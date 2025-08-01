#include "dnswriter.hh"
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "packethandler.hh"
#include "qtype.hh"
#include "dnspacket.hh"
#include "auth-caches.hh"
#include "statbag.hh"
#include "dnsseckeeper.hh"
#include "base64.hh"
#include "base32.hh"

#include "misc.hh"
#include "arguments.hh"
#include "resolver.hh"
#include "dns_random.hh"
#include "backends/gsql/ssql.hh"
#include "communicator.hh"
#include "query-local-address.hh"
#include "gss_context.hh"
#include "auth-main.hh"

extern StatBag S;
extern CommunicatorClass Communicator;

std::mutex PacketHandler::s_rfc2136lock;

// Implement section 3.2.1 and 3.2.2 of RFC2136
int PacketHandler::checkUpdatePrerequisites(const DNSRecord *rr, DomainInfo *di) {
  if (rr->d_ttl != 0)
    return RCode::FormErr;

  // 3.2.1 and 3.2.2 check content length.
  if ( (rr->d_class == QClass::NONE || rr->d_class == QClass::ANY) && rr->d_clen != 0)
    return RCode::FormErr;

  bool foundRecord=false;
  DNSResourceRecord rec;
  di->backend->lookup(QType(QType::ANY), rr->d_name, di->id);
  while(di->backend->get(rec)) {
    if (!rec.qtype.getCode())
      continue;
    if ((rr->d_type != QType::ANY && rec.qtype == rr->d_type) || rr->d_type == QType::ANY)
      foundRecord=true;
  }

  // Section 3.2.1
  if (rr->d_class == QClass::ANY && !foundRecord) {
    if (rr->d_type == QType::ANY)
      return RCode::NXDomain;
    if (rr->d_type != QType::ANY)
      return RCode::NXRRSet;
  }

  // Section 3.2.2
  if (rr->d_class == QClass::NONE && foundRecord) {
    if (rr->d_type == QType::ANY)
      return RCode::YXDomain;
    if (rr->d_type != QType::ANY)
      return RCode::YXRRSet;
  }

  return RCode::NoError;
}


// Method implements section 3.4.1 of RFC2136
int PacketHandler::checkUpdatePrescan(const DNSRecord *rr) {
  // The RFC stats that d_class != ZCLASS, but we only support the IN class.
  if (rr->d_class != QClass::IN && rr->d_class != QClass::NONE && rr->d_class != QClass::ANY) {
    return RCode::FormErr;
  }

  QType qtype = QType(rr->d_type);

  if (!qtype.isSupportedType()) {
    return RCode::FormErr;
  }

  if ((rr->d_class == QClass::NONE || rr->d_class == QClass::ANY) && rr->d_ttl != 0) {
    return RCode::FormErr;
  }

  if (rr->d_class == QClass::ANY && rr->d_clen != 0) {
    return RCode::FormErr;
  }

  if (qtype.isMetadataType()) {
    return RCode::FormErr;
  }

  if (rr->d_class != QClass::ANY && qtype.getCode() == QType::ANY) {
    return RCode::FormErr;
  }

  return RCode::NoError;
}


// Implements section 3.4.2 of RFC2136
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
uint PacketHandler::performUpdate(const string &msgPrefix, const DNSRecord *rr, DomainInfo *di, bool isPresigned, bool* narrow, bool* haveNSEC3, NSEC3PARAMRecordContent *ns3pr, bool *updatedSerial) {

  QType rrType = QType(rr->d_type);

  if (rrType == QType::NSEC || rrType == QType::NSEC3) {
    g_log<<Logger::Warning<<msgPrefix<<"Trying to add/update/delete "<<rr->d_name<<"|"<<rrType.toString()<<". These are generated records, ignoring!"<<endl;
    return 0;
  }

  if (!isPresigned && rrType == QType::RRSIG) {
    g_log<<Logger::Warning<<msgPrefix<<"Trying to add/update/delete "<<rr->d_name<<"|"<<rrType.toString()<<" in non-presigned zone, ignoring!"<<endl;
    return 0;
  }

  if ((rrType == QType::NSEC3PARAM || rrType == QType::DNSKEY) && rr->d_name != di->zone.operator const DNSName&()) {
    g_log<<Logger::Warning<<msgPrefix<<"Trying to add/update/delete "<<rr->d_name<<"|"<<rrType.toString()<<", "<<rrType.toString()<<" must be at zone apex, ignoring!"<<endl;
    return 0;
  }


  uint changedRecords = 0;
  DNSResourceRecord rec;
  vector<DNSResourceRecord> rrset, recordsToDelete;
  set<DNSName> delnonterm, insnonterm; // used to (at the end) fix ENT records.


  if (rr->d_class == QClass::IN) { // 3.4.2.2 QClass::IN means insert or update
    DLOG(g_log<<msgPrefix<<"Add/Update record (QClass == IN) "<<rr->d_name<<"|"<<rrType.toString()<<endl);

    if (rrType == QType::NSEC3PARAM) {
      g_log<<Logger::Notice<<msgPrefix<<"Adding/updating NSEC3PARAM for zone, resetting ordernames."<<endl;

      *ns3pr = NSEC3PARAMRecordContent(rr->getContent()->getZoneRepresentation(), di->zone);
      *narrow = false; // adding a NSEC3 will cause narrow mode to be dropped, as you cannot specify that in a NSEC3PARAM record
      d_dk.setNSEC3PARAM(di->zone, *ns3pr, (*narrow));
      *haveNSEC3 = true;

      string error;
      string info;
      if (!d_dk.rectifyZone(di->zone, error, info, false)) {
        throw PDNSException("Failed to rectify '" + di->zone.toLogString() + "': " + error);
      }
      return 1;
    }



    bool foundRecord = false;
    di->backend->lookup(rrType, rr->d_name, di->id);
    while (di->backend->get(rec)) {
      rrset.push_back(rec);
      foundRecord = true;
    }

    if (foundRecord) {

      if (rrType == QType::SOA) { // SOA updates require the serial to be higher than the current
        SOAData sdOld, sdUpdate;
        DNSResourceRecord *oldRec = &rrset.front();
        fillSOAData(oldRec->content, sdOld);
        oldRec->setContent(rr->getContent()->getZoneRepresentation());
        fillSOAData(oldRec->content, sdUpdate);
        if (rfc1982LessThan(sdOld.serial, sdUpdate.serial)) {
          di->backend->replaceRRSet(di->id, oldRec->qname, oldRec->qtype, rrset);
          *updatedSerial = true;
          changedRecords++;
          g_log<<Logger::Notice<<msgPrefix<<"Replacing SOA record "<<rr->d_name<<"|"<<rrType.toString()<<endl;
        } else {
          g_log<<Logger::Notice<<msgPrefix<<"Provided serial ("<<sdUpdate.serial<<") is older than the current serial ("<<sdOld.serial<<"), ignoring SOA update."<<endl;
        }

      // It's not possible to have multiple CNAME's with the same NAME. So we always update.
      } else if (rrType == QType::CNAME) {
        int changedCNames = 0;
        for (auto& i : rrset) {
          if (i.ttl != rr->d_ttl || i.content != rr->getContent()->getZoneRepresentation()) {
            i.ttl = rr->d_ttl;
            i.setContent(rr->getContent()->getZoneRepresentation());
            changedCNames++;
          }
        }
        if (changedCNames > 0) {
          di->backend->replaceRRSet(di->id, rr->d_name, rrType, rrset);
          g_log<<Logger::Notice<<msgPrefix<<"Replacing CNAME record "<<rr->d_name<<"|"<<rrType.toString()<<endl;
          changedRecords += changedCNames;
        } else {
          g_log<<Logger::Notice<<msgPrefix<<"Replace for CNAME record "<<rr->d_name<<"|"<<rrType.toString()<<" requested, but no changes made."<<endl;
        }

      // In any other case, we must check if the TYPE and RDATA match to provide an update (which effectively means a update of TTL)
      } else {
        int updateTTL=0;
        foundRecord = false;
        bool lowerCase = false;
        if (rrType.getCode() == QType::PTR ||
            rrType.getCode() == QType::MX ||
            rrType.getCode() == QType::SRV) {
          lowerCase = true;
        }
        string content = rr->getContent()->getZoneRepresentation();
        if (lowerCase) content = toLower(content);
        for (auto& i : rrset) {
          string icontent = i.getZoneRepresentation();
          if (lowerCase) icontent = toLower(icontent);
          if (rrType == i.qtype.getCode()) {
            if (icontent == content) {
              foundRecord=true;
            }
            if (i.ttl != rr->d_ttl)  {
              i.ttl = rr->d_ttl;
              updateTTL++;
            }
          }
        }
        if (updateTTL > 0) {
          di->backend->replaceRRSet(di->id, rr->d_name, rrType, rrset);
          g_log<<Logger::Notice<<msgPrefix<<"Updating TTLs for "<<rr->d_name<<"|"<<rrType.toString()<<endl;
          changedRecords += updateTTL;
        } else {
          g_log<<Logger::Notice<<msgPrefix<<"Replace for recordset "<<rr->d_name<<"|"<<rrType.toString()<<" requested, but no changes made."<<endl;
        }
      }

      // ReplaceRRSet dumps our ordername and auth flag, so we need to correct it if we have changed records.
      // We can take the auth flag from the first RR in the set, as the name is different, so should the auth be.
      if (changedRecords > 0) {
        bool auth = rrset.front().auth;

        if(*haveNSEC3) {
          DNSName ordername;
          if(! *narrow)
            ordername=DNSName(toBase32Hex(hashQNameWithSalt(*ns3pr, rr->d_name)));

          if (*narrow) {
            di->backend->updateDNSSECOrderNameAndAuth(di->id, rr->d_name, DNSName(), auth, QType::ANY, true);
	  }
          else {
            di->backend->updateDNSSECOrderNameAndAuth(di->id, rr->d_name, ordername, auth, QType::ANY, true);
	  }
          if(!auth || rrType == QType::DS) {
            di->backend->updateDNSSECOrderNameAndAuth(di->id, rr->d_name, DNSName(), false, QType::NS, true);
            di->backend->updateDNSSECOrderNameAndAuth(di->id, rr->d_name, DNSName(), false, QType::A, true);
            di->backend->updateDNSSECOrderNameAndAuth(di->id, rr->d_name, DNSName(), false, QType::AAAA, true);
          }

        } else { // NSEC
          di->backend->updateDNSSECOrderNameAndAuth(di->id, rr->d_name, rr->d_name.makeRelative(di->zone), auth, QType::ANY, false);
          if(!auth || rrType == QType::DS) {
            di->backend->updateDNSSECOrderNameAndAuth(di->id, rr->d_name, DNSName(), false, QType::A, false);
            di->backend->updateDNSSECOrderNameAndAuth(di->id, rr->d_name, DNSName(), false, QType::AAAA, false);
          }
        }
      }

    } // if (foundRecord)

    // If we haven't found a record that matches, we must add it.
    if (! foundRecord) {
      g_log<<Logger::Notice<<msgPrefix<<"Adding record "<<rr->d_name<<"|"<<rrType.toString()<<endl;
      delnonterm.insert(rr->d_name); // always remove any ENT's in the place where we're going to add a record.
      auto newRec = DNSResourceRecord::fromWire(*rr);
      newRec.domain_id = di->id;
      newRec.auth = (rr->d_name == di->zone.operator const DNSName&() || rrType.getCode() != QType::NS);
      di->backend->feedRecord(newRec, DNSName());
      changedRecords++;


      // because we added a record, we need to fix DNSSEC data.
      ZoneName shorter(rr->d_name);
      bool auth=newRec.auth;
      bool fixDS = (rrType == QType::DS);

      if (di->zone != shorter) { // Everything at APEX is auth=1 && no ENT's
        do {

          if (di->zone == shorter)
            break;

          bool foundShorter = false;
          di->backend->lookup(QType(QType::ANY), shorter.operator const DNSName&(), di->id);
          while (di->backend->get(rec)) {
            if (rec.qname == rr->d_name && rec.qtype == QType::DS)
              fixDS = true;
            if (shorter.operator const DNSName&() != rr->d_name) {
              foundShorter = true;
            }
            if (rec.qtype == QType::NS) // are we inserting below a delegate?
              auth=false;
          }

          if (!foundShorter && auth && shorter.operator const DNSName&() != rr->d_name) { // haven't found any record at current level, insert ENT.
            insnonterm.insert(shorter.operator const DNSName&());
          }
          if (foundShorter)
            break; // if we find a shorter record, we can stop searching
        } while(shorter.chopOff());
      }

      if(*haveNSEC3)
      {
        DNSName ordername;
        if(! *narrow)
          ordername=DNSName(toBase32Hex(hashQNameWithSalt(*ns3pr, rr->d_name)));

        if (*narrow) {
          di->backend->updateDNSSECOrderNameAndAuth(di->id, rr->d_name, DNSName(), auth, QType::ANY, true);
	}
        else {
          di->backend->updateDNSSECOrderNameAndAuth(di->id, rr->d_name, ordername, auth, QType::ANY, true);
	}

        if (fixDS) {
          di->backend->updateDNSSECOrderNameAndAuth(di->id, rr->d_name, ordername, true, QType::DS, true);
	}

        if(!auth) {
          if (ns3pr->d_flags != 0) {
            di->backend->updateDNSSECOrderNameAndAuth(di->id, rr->d_name, DNSName(), false, QType::NS, true);
	  }
          di->backend->updateDNSSECOrderNameAndAuth(di->id, rr->d_name, DNSName(), false, QType::A, true);
          di->backend->updateDNSSECOrderNameAndAuth(di->id, rr->d_name, DNSName(), false, QType::AAAA, true);
        }
      }
      else { // NSEC
        DNSName ordername=rr->d_name.makeRelative(di->zone);
        di->backend->updateDNSSECOrderNameAndAuth(di->id, rr->d_name, ordername, auth, QType::ANY, false);
        if (fixDS) {
          di->backend->updateDNSSECOrderNameAndAuth(di->id, rr->d_name, ordername, true, QType::DS, false);
        }
        if(!auth) {
          di->backend->updateDNSSECOrderNameAndAuth(di->id, rr->d_name, DNSName(), false, QType::A, false);
          di->backend->updateDNSSECOrderNameAndAuth(di->id, rr->d_name, DNSName(), false, QType::AAAA, false);
        }
      }


      // If we insert an NS, all the records below it become non auth - so, we're inserting a delegate.
      // Auth can only be false when the rr->d_name is not the zone
      if (auth == false && rrType == QType::NS) {
        DLOG(g_log<<msgPrefix<<"Going to fix auth flags below "<<rr->d_name<<endl);
        insnonterm.clear(); // No ENT's are needed below delegates (auth=0)
        vector<DNSName> qnames;
        di->backend->listSubZone(ZoneName(rr->d_name), di->id);
        while(di->backend->get(rec)) {
          if (rec.qtype.getCode() && rec.qtype.getCode() != QType::DS && rr->d_name != rec.qname) // Skip ENT, DS and our already corrected record.
            qnames.push_back(rec.qname);
        }
        for(const auto & qname : qnames) {
          if(*haveNSEC3)  {
            DNSName ordername;
            if(! *narrow)
              ordername=DNSName(toBase32Hex(hashQNameWithSalt(*ns3pr, qname)));

            if (*narrow) {
              di->backend->updateDNSSECOrderNameAndAuth(di->id, qname, DNSName(), auth, QType::ANY, true);
	    }
            else {
              di->backend->updateDNSSECOrderNameAndAuth(di->id, qname, ordername, auth, QType::ANY, true);
	    }

            if (ns3pr->d_flags != 0) {
              di->backend->updateDNSSECOrderNameAndAuth(di->id, qname, DNSName(), false, QType::NS, true);
	    }
          }
          else { // NSEC
            DNSName ordername=DNSName(qname).makeRelative(di->zone);
            di->backend->updateDNSSECOrderNameAndAuth(di->id, qname, ordername, false, QType::NS, false);
          }

          di->backend->updateDNSSECOrderNameAndAuth(di->id, qname, DNSName(), false, QType::A, *haveNSEC3);
          di->backend->updateDNSSECOrderNameAndAuth(di->id, qname, DNSName(), false, QType::AAAA, *haveNSEC3);
        }
      }
    }
  } // rr->d_class == QClass::IN


  // Delete records - section 3.4.2.3 and 3.4.2.4 with the exception of the 'always leave 1 NS rule' as that's handled by
  // the code that calls this performUpdate().
  if ((rr->d_class == QClass::ANY || rr->d_class == QClass::NONE) && rrType != QType::SOA) { // never delete a SOA.
    DLOG(g_log<<msgPrefix<<"Deleting records: "<<rr->d_name<<"; QClass:"<<rr->d_class<<"; rrType: "<<rrType.toString()<<endl);

    if (rrType == QType::NSEC3PARAM) {
      g_log<<Logger::Notice<<msgPrefix<<"Deleting NSEC3PARAM from zone, resetting ordernames."<<endl;
      // Be sure to use a ZoneName with a variant matching the domain we are
      // working on, for the sake of unsetNSEC3PARAM.
      ZoneName zonename(rr->d_name, di->zone.getVariant());
      if (rr->d_class == QClass::ANY) {
        d_dk.unsetNSEC3PARAM(zonename);
      }
      else if (rr->d_class == QClass::NONE) {
        NSEC3PARAMRecordContent nsec3rr(rr->getContent()->getZoneRepresentation(), di->zone);
        if (*haveNSEC3 && ns3pr->getZoneRepresentation() == nsec3rr.getZoneRepresentation())
          d_dk.unsetNSEC3PARAM(zonename);
        else
          return 0;
      } else
        return 0;

      // Update NSEC3 variables, other RR's in this update package might need them as well.
      *haveNSEC3 = false;
      *narrow = false;

      string error;
      string info;
      if (!d_dk.rectifyZone(di->zone, error, info, false)) {
        throw PDNSException("Failed to rectify '" + di->zone.toLogString() + "': " + error);
      }
      return 1;
    } // end of NSEC3PARAM delete block


    di->backend->lookup(rrType, rr->d_name, di->id);
    while(di->backend->get(rec)) {
      if (rr->d_class == QClass::ANY) { // 3.4.2.3
        if (rec.qname == di->zone.operator const DNSName&() && (rec.qtype == QType::NS || rec.qtype == QType::SOA)) { // Never delete all SOA and NS's
          rrset.push_back(rec);
        }
        else
          recordsToDelete.push_back(rec);
      }
      if (rr->d_class == QClass::NONE) { // 3.4.2.4
        auto repr = rec.getZoneRepresentation();
        if (rec.qtype == QType::TXT) {
          DLOG(g_log<<msgPrefix<<"Adjusting TXT content from ["<<repr<<"]"<<endl);
          auto drc = DNSRecordContent::make(rec.qtype.getCode(), QClass::IN, repr);
          auto ser = drc->serialize(rec.qname, true, true);
          auto rc = DNSRecordContent::deserialize(rec.qname, rec.qtype.getCode(), ser);
          repr = rc->getZoneRepresentation(true);
          DLOG(g_log<<msgPrefix<<"Adjusted TXT content to ["<<repr<<"]"<<endl);
        }
        DLOG(g_log<<msgPrefix<<"Matching RR in RRset - (adjusted) representation from request=["<<repr<<"], rr->getContent()->getZoneRepresentation()=["<<rr->getContent()->getZoneRepresentation()<<"]"<<endl);
        if (rrType == rec.qtype && repr == rr->getContent()->getZoneRepresentation())
          recordsToDelete.push_back(rec);
        else
          rrset.push_back(rec);
      }
    }
  
    if (recordsToDelete.size()) {
      di->backend->replaceRRSet(di->id, rr->d_name, rrType, rrset);
      g_log<<Logger::Notice<<msgPrefix<<"Deleting record "<<rr->d_name<<"|"<<rrType.toString()<<endl;
      changedRecords += recordsToDelete.size();


      // If we've removed a delegate, we need to reset ordername/auth for some records.
      if (rrType == QType::NS && rr->d_name != di->zone.operator const DNSName&()) { 
        vector<DNSName> belowOldDelegate, nsRecs, updateAuthFlag;
        di->backend->listSubZone(ZoneName(rr->d_name), di->id);
        while (di->backend->get(rec)) {
          if (rec.qtype.getCode()) // skip ENT records, they are always auth=false
            belowOldDelegate.push_back(rec.qname);
          if (rec.qtype.getCode() == QType::NS && rec.qname != rr->d_name)
            nsRecs.push_back(rec.qname);
        }

        for(auto &belowOldDel: belowOldDelegate)
        {
          bool isBelowDelegate = false;
          for(const auto & ns: nsRecs) {
            if (ns.isPartOf(belowOldDel)) {
              isBelowDelegate=true;
              break;
            }
          }
          if (!isBelowDelegate)
            updateAuthFlag.push_back(belowOldDel);
        }

        for (const auto &changeRec:updateAuthFlag) {
          DNSName ordername;
          if(*haveNSEC3)  {
            if(! *narrow) {
              ordername=DNSName(toBase32Hex(hashQNameWithSalt(*ns3pr, changeRec)));
	    }
          }
          else { // NSEC
            ordername=changeRec.makeRelative(di->zone);
          }
          di->backend->updateDNSSECOrderNameAndAuth(di->id, changeRec, ordername, true, QType::ANY, *haveNSEC3);
        }
      }

      // Fix ENT records.
      // We must check if we have a record below the current level and if we removed the 'last' record
      // on that level. If so, we must insert an ENT record.
      // We take extra care here to not 'include' the record that we just deleted. Some backends will still return it as they only reload on a commit.
      bool foundDeeper = false, foundOtherWithSameName = false;
      di->backend->listSubZone(ZoneName(rr->d_name), di->id);
      while (di->backend->get(rec)) {
        if (rec.qname == rr->d_name && !count(recordsToDelete.begin(), recordsToDelete.end(), rec))
          foundOtherWithSameName = true;
        if (rec.qname != rr->d_name && rec.qtype.getCode() != QType::NS) //Skip NS records, as this would be a delegate that we can ignore as this does not require us to create a ENT
          foundDeeper = true;
      }

      if (foundDeeper && !foundOtherWithSameName) {
        insnonterm.insert(rr->d_name);
      } else if (!foundOtherWithSameName) {
        // If we didn't have to insert an ENT, we might have deleted a record at very deep level
        // and we must then clean up the ENT's above the deleted record.
        ZoneName shorter(rr->d_name);
        while (shorter != di->zone) {
          shorter.chopOff();
          bool foundRealRR = false;
          bool foundEnt = false;

          // The reason for a listSubZone here is because might go up the tree and find the ENT of another branch
          // consider these non ENT-records:
          // b.c.d.e.test.com
          // b.d.e.test.com
          // if we delete b.c.d.e.test.com, we go up to d.e.test.com and then find b.d.e.test.com because that's below d.e.test.com.
          // At that point we can stop deleting ENT's because the tree is in tact again.
          di->backend->listSubZone(shorter, di->id);

          while (di->backend->get(rec)) {
            if (rec.qtype.getCode())
              foundRealRR = true;
            else
              foundEnt = true;
          }
          if (!foundRealRR) {
            if (foundEnt) // only delete the ENT if we actually found one.
              delnonterm.insert(shorter.operator const DNSName&());
          } else
            break;
        }
      }
    } else { // if (recordsToDelete.size())
      g_log<<Logger::Notice<<msgPrefix<<"Deletion for record "<<rr->d_name<<"|"<<rrType.toString()<<" requested, but not found."<<endl;
    }
  } // (End of delete block d_class == ANY || d_class == NONE
  


  //Insert and delete ENT's
  if (insnonterm.size() > 0 || delnonterm.size() > 0) {
    DLOG(g_log<<msgPrefix<<"Updating ENT records - "<<insnonterm.size()<<"|"<<delnonterm.size()<<endl);
    di->backend->updateEmptyNonTerminals(di->id, insnonterm, delnonterm, false);
    for (const auto &i: insnonterm) {
      string hashed;
      if(*haveNSEC3)
      {
        DNSName ordername;
        if(! *narrow) {
          ordername=DNSName(toBase32Hex(hashQNameWithSalt(*ns3pr, i)));
	}
        di->backend->updateDNSSECOrderNameAndAuth(di->id, i, ordername, true, QType::ANY, true);
      }
    }
  }

  return changedRecords;
}

int PacketHandler::forwardPacket(const string &msgPrefix, const DNSPacket& p, const DomainInfo& di) {
  vector<string> forward;
  B.getDomainMetadata(p.qdomainzone, "FORWARD-DNSUPDATE", forward);

  if (forward.size() == 0 && ! ::arg().mustDo("forward-dnsupdate")) {
    g_log << Logger::Notice << msgPrefix << "Not configured to forward to primary, returning Refused." << endl;
    return RCode::Refused;
  }

  for (const auto& remote : di.primaries) {
    g_log << Logger::Notice << msgPrefix << "Forwarding packet to primary " << remote << endl;

    if (!pdns::isQueryLocalAddressFamilyEnabled(remote.sin4.sin_family)) {
      continue;
    }
    auto local = pdns::getQueryLocalAddress(remote.sin4.sin_family, 0);
    int sock = makeQuerySocket(local, false); // create TCP socket. RFC2136 section 6.2 seems to be ok with this.
    if(sock < 0) {
      g_log<<Logger::Error<<msgPrefix<<"Error creating socket: "<<stringerror()<<endl;
      continue;
    }

    if( connect(sock, (struct sockaddr*)&remote, remote.getSocklen()) < 0 ) {
      g_log<<Logger::Error<<msgPrefix<<"Failed to connect to "<<remote.toStringWithPort()<<": "<<stringerror()<<endl;
      try {
        closesocket(sock);
      }
      catch(const PDNSException& e) {
        g_log << Logger::Error << "Error closing primary forwarding socket after connect() failed: " << e.reason << endl;
      }
      continue;
    }

    DNSPacket l_forwardPacket(p);
    l_forwardPacket.setID(dns_random_uint16());
    l_forwardPacket.setRemote(&remote);
    uint16_t len=htons(l_forwardPacket.getString().length());
    string buffer((const char*)&len, 2);
    buffer.append(l_forwardPacket.getString());
    if(write(sock, buffer.c_str(), buffer.length()) < 0) {
      g_log<<Logger::Error<<msgPrefix<<"Unable to forward update message to "<<remote.toStringWithPort()<<", error:"<<stringerror()<<endl;
      try {
        closesocket(sock);
      }
      catch(const PDNSException& e) {
        g_log << Logger::Error << "Error closing primary forwarding socket after write() failed: " << e.reason << endl;
      }
      continue;
    }

    int res = waitForData(sock, 10, 0);
    if (!res) {
      g_log << Logger::Error << msgPrefix << "Timeout waiting for reply from primary at " << remote.toStringWithPort() << endl;
      try {
        closesocket(sock);
      }
      catch(const PDNSException& e) {
        g_log << Logger::Error << "Error closing primary forwarding socket after a timeout occurred: " << e.reason << endl;
      }
      continue;
    }
    if (res < 0) {
      g_log << Logger::Error << msgPrefix << "Error waiting for answer from primary at " << remote.toStringWithPort() << ", error:" << stringerror() << endl;
      try {
        closesocket(sock);
      }
      catch(const PDNSException& e) {
        g_log << Logger::Error << "Error closing primary forwarding socket after an error occurred: " << e.reason << endl;
      }
      continue;
    }

    unsigned char lenBuf[2];
    ssize_t recvRes;
    recvRes = recv(sock, &lenBuf, sizeof(lenBuf), 0);
    if (recvRes < 0 || static_cast<size_t>(recvRes) < sizeof(lenBuf)) {
      g_log << Logger::Error << msgPrefix << "Could not receive data (length) from primary at " << remote.toStringWithPort() << ", error:" << stringerror() << endl;
      try {
        closesocket(sock);
      }
      catch(const PDNSException& e) {
        g_log << Logger::Error << "Error closing primary forwarding socket after recv() failed: " << e.reason << endl;
      }
      continue;
    }
    size_t packetLen = lenBuf[0]*256+lenBuf[1];

    buffer.resize(packetLen);
    recvRes = recv(sock, &buffer.at(0), packetLen, 0);
    if (recvRes < 0) {
      g_log << Logger::Error << msgPrefix << "Could not receive data (dnspacket) from primary at " << remote.toStringWithPort() << ", error:" << stringerror() << endl;
      try {
        closesocket(sock);
      }
      catch(const PDNSException& e) {
        g_log << Logger::Error << "Error closing primary forwarding socket after recv() failed: " << e.reason << endl;
      }
      continue;
    }
    try {
      closesocket(sock);
    }
    catch(const PDNSException& e) {
      g_log << Logger::Error << "Error closing primary forwarding socket: " << e.reason << endl;
    }

    try {
      MOADNSParser mdp(false, buffer.data(), static_cast<unsigned int>(recvRes));
      g_log<<Logger::Info<<msgPrefix<<"Forward update message to "<<remote.toStringWithPort()<<", result was RCode "<<mdp.d_header.rcode<<endl;
      return mdp.d_header.rcode;
    }
    catch (...) {
      g_log << Logger::Error << msgPrefix << "Failed to parse response packet from primary at " << remote.toStringWithPort() << endl;
      continue;
    }
  }
  g_log << Logger::Error << msgPrefix << "Failed to forward packet to primary(s). Returning ServFail." << endl;
  return RCode::ServFail;

}

int PacketHandler::processUpdate(DNSPacket& packet) { // NOLINT(readability-function-cognitive-complexity)
  if (! ::arg().mustDo("dnsupdate"))
    return RCode::Refused;

  string msgPrefix="UPDATE (" + std::to_string(packet.d.id) + ") from " + packet.getRemoteString() + " for " + packet.qdomainzone.toLogString() + ": ";
  g_log<<Logger::Info<<msgPrefix<<"Processing started."<<endl;

  // if there is policy, we delegate all checks to it
  if (this->d_update_policy_lua == nullptr) {

    // Check permissions - IP based
    vector<string> allowedRanges;
    B.getDomainMetadata(packet.qdomainzone, "ALLOW-DNSUPDATE-FROM", allowedRanges);
    if (! ::arg()["allow-dnsupdate-from"].empty())
      stringtok(allowedRanges, ::arg()["allow-dnsupdate-from"], ", \t" );

    NetmaskGroup ng;
    for(const auto& i: allowedRanges) {
      ng.addMask(i);
    }

    if ( ! ng.match(packet.getInnerRemote())) {
      g_log<<Logger::Error<<msgPrefix<<"Remote not listed in allow-dnsupdate-from or domainmetadata. Sending REFUSED"<<endl;
      return RCode::Refused;
    }


    // Check permissions - TSIG based.
    vector<string> tsigKeys;
    B.getDomainMetadata(packet.qdomainzone, "TSIG-ALLOW-DNSUPDATE", tsigKeys);
    if (tsigKeys.size() > 0) {
      bool validKey = false;

      TSIGRecordContent trc;
      DNSName inputkey;
      string message;
      if (! packet.getTSIGDetails(&trc,  &inputkey)) {
        g_log<<Logger::Error<<msgPrefix<<"TSIG key required, but packet does not contain key. Sending REFUSED"<<endl;
        return RCode::Refused;
      }
#ifdef ENABLE_GSS_TSIG
      if (g_doGssTSIG && packet.d_tsig_algo == TSIG_GSS) {
        GssName inputname(packet.d_peer_principal); // match against principal since GSS requires that
        for(const auto& key: tsigKeys) {
          if (inputname.match(key)) {
            validKey = true;
            break;
          }
        }
      }
      else
#endif
        {
        for(const auto& key: tsigKeys) {
          if (inputkey == DNSName(key)) { // because checkForCorrectTSIG has already been performed earlier on, if the name of the key matches with the domain given it is valid.
            validKey=true;
            break;
          }
        }
      }

      if (!validKey) {
        g_log<<Logger::Error<<msgPrefix<<"TSIG key ("<<inputkey<<") required, but no matching key found in domainmetadata, tried "<<tsigKeys.size()<<". Sending REFUSED"<<endl;
        return RCode::Refused;
      }
    } else if(::arg().mustDo("dnsupdate-require-tsig")) {
      g_log<<Logger::Error<<msgPrefix<<"TSIG key required, but domain is not secured with TSIG. Sending REFUSED"<<endl;
      return RCode::Refused;
    }

    if (tsigKeys.empty() && packet.d_havetsig) {
      g_log<<Logger::Warning<<msgPrefix<<"TSIG is provided, but domain is not secured with TSIG. Processing continues"<<endl;
    }

  }

  // RFC2136 uses the same DNS Header and Message as defined in RFC1035.
  // This means we can use the MOADNSParser to parse the incoming packet. The result is that we have some different
  // variable names during the use of our MOADNSParser.
  MOADNSParser mdp(false, packet.getString());
  if (mdp.d_header.qdcount != 1) {
    g_log<<Logger::Warning<<msgPrefix<<"Zone Count is not 1, sending FormErr"<<endl;
    return RCode::FormErr;
  }

  if (packet.qtype.getCode() != QType::SOA) { // RFC2136 2.3 - ZTYPE must be SOA
    g_log<<Logger::Warning<<msgPrefix<<"Query ZTYPE is not SOA, sending FormErr"<<endl;
    return RCode::FormErr;
  }

  if (packet.qclass != QClass::IN) {
    g_log<<Logger::Warning<<msgPrefix<<"Class is not IN, sending NotAuth"<<endl;
    return RCode::NotAuth;
  }

  DomainInfo di;
  di.backend=nullptr;
  if(!B.getDomainInfo(packet.qdomainzone, di) || (di.backend == nullptr)) {
    g_log<<Logger::Error<<msgPrefix<<"Can't determine backend for domain '"<<packet.qdomainzone<<"' (or backend does not support DNS update operation)"<<endl;
    return RCode::NotAuth;
  }

  if (di.kind == DomainInfo::Secondary)
    return forwardPacket(msgPrefix, packet, di);

  // Check if all the records provided are within the zone
  for(const auto & answer : mdp.d_answers) {
    const DNSRecord *dnsRecord = &answer;
    // Skip this check for other field types (like the TSIG -  which is in the additional section)
    // For a TSIG, the label is the dnskey, so it does not pass the endOn validation.
    if (dnsRecord->d_place != DNSResourceRecord::ANSWER && dnsRecord->d_place != DNSResourceRecord::AUTHORITY) {
      continue;
    }

    if (!dnsRecord->d_name.isPartOf(di.zone)) {
      g_log<<Logger::Error<<msgPrefix<<"Received update/record out of zone, sending NotZone."<<endl;
      return RCode::NotZone;
    }
  }


  auto lock = std::scoped_lock(s_rfc2136lock); //TODO: i think this lock can be per zone, not for everything
  g_log<<Logger::Info<<msgPrefix<<"starting transaction."<<endl;
  if (!di.backend->startTransaction(packet.qdomainzone, UnknownDomainID)) { // Not giving the domain_id means that we do not delete the existing records.
    g_log<<Logger::Error<<msgPrefix<<"Backend for domain "<<packet.qdomainzone<<" does not support transaction. Can't do Update packet."<<endl;
    return RCode::NotImp;
  }

  // 3.2.1 and 3.2.2 - Prerequisite check
  for(const auto & answer : mdp.d_answers) {
    const DNSRecord *dnsRecord = &answer;
    if (dnsRecord->d_place == DNSResourceRecord::ANSWER) {
      int res = checkUpdatePrerequisites(dnsRecord, &di);
      if (res>0) {
        g_log<<Logger::Error<<msgPrefix<<"Failed PreRequisites check for "<<dnsRecord->d_name<<", returning "<<RCode::to_s(res)<<endl;
        di.backend->abortTransaction();
        return res;
      }
    }
  }

  // 3.2.3 - Prerequisite check - this is outside of updatePrerequisitesCheck because we check an RRSet and not the RR.
  typedef pair<DNSName, QType> rrSetKey_t;
  typedef vector<DNSResourceRecord> rrVector_t;
  typedef std::map<rrSetKey_t, rrVector_t> RRsetMap_t;
  RRsetMap_t preReqRRsets;
  for(const auto& i: mdp.d_answers) {
    const DNSRecord* dnsRecord = &i;
    if (dnsRecord->d_place == DNSResourceRecord::ANSWER) {
      // Last line of 3.2.3
      if (dnsRecord->d_class != QClass::IN && dnsRecord->d_class != QClass::NONE && dnsRecord->d_class != QClass::ANY) {
        di.backend->abortTransaction();
        return RCode::FormErr;
      }

      if (dnsRecord->d_class == QClass::IN) {
        rrSetKey_t key = {dnsRecord->d_name, QType(dnsRecord->d_type)};
        rrVector_t *vec = &preReqRRsets[key];
        vec->push_back(DNSResourceRecord::fromWire(*dnsRecord));
      }
    }
  }

  if (preReqRRsets.size() > 0) {
    RRsetMap_t zoneRRsets;
    for (auto & preReqRRset : preReqRRsets) {
      rrSetKey_t rrSet=preReqRRset.first;
      rrVector_t *vec = &preReqRRset.second;

      DNSResourceRecord rec;
      di.backend->lookup(QType(QType::ANY), rrSet.first, di.id);
      uint16_t foundRR=0, matchRR=0;
      while (di.backend->get(rec)) {
        if (rec.qtype == rrSet.second) {
          foundRR++;
          for(auto & rrItem : *vec) {
            rrItem.ttl = rec.ttl; // The compare one line below also compares TTL, so we make them equal because TTL is not user within prerequisite checks.
            if (rrItem == rec)
              matchRR++;
          }
        }
      }
      if (matchRR != foundRR || foundRR != vec->size()) {
        g_log<<Logger::Error<<msgPrefix<<"Failed PreRequisites check (RRs differ), returning NXRRSet"<<endl;
        di.backend->abortTransaction();
        return RCode::NXRRSet;
      }
    }
  }



  // 3.4 - Prescan & Add/Update/Delete records - is all done within a try block.
  try {
    uint changedRecords = 0;
    // 3.4.1 - Prescan section
    for(const auto & answer : mdp.d_answers) {
      const DNSRecord *dnsRecord = &answer;
      if (dnsRecord->d_place == DNSResourceRecord::AUTHORITY) {
        int res = checkUpdatePrescan(dnsRecord);
        if (res>0) {
          g_log<<Logger::Error<<msgPrefix<<"Failed prescan check, returning "<<res<<endl;
          di.backend->abortTransaction();
          return res;
        }
      }
    }

    bool updatedSerial=false;
    NSEC3PARAMRecordContent ns3pr;
    bool narrow=false;
    bool haveNSEC3 = d_dk.getNSEC3PARAM(di.zone, &ns3pr, &narrow);
    bool isPresigned = d_dk.isPresigned(di.zone);
    string soaEditSetting;
    d_dk.getSoaEdit(di.zone, soaEditSetting);

    // 3.4.2 - Perform the updates.
    // There's a special condition where deleting the last NS record at zone apex is never deleted (3.4.2.4)
    // This means we must do it outside the normal performUpdate() because that focusses only on a separate RR.
    vector<const DNSRecord *> nsRRtoDelete;

    // Another special case is the addition of both a CNAME and a non-CNAME for the same name (#6270)
    set<DNSName> cn, nocn;
    for (const auto &rr : mdp.d_answers) {
      if (rr.d_place == DNSResourceRecord::AUTHORITY && rr.d_class == QClass::IN && rr.d_ttl > 0) {
        // Addition
        if (rr.d_type == QType::CNAME) {
          cn.insert(rr.d_name);
        } else if (rr.d_type != QType::RRSIG) {
          nocn.insert(rr.d_name);
        }
      }
    }
    for (auto const &n : cn) {
      if (nocn.count(n) > 0) {
        g_log<<Logger::Error<<msgPrefix<<"Refusing update, found CNAME and non-CNAME addition"<<endl;
        di.backend->abortTransaction();
        return RCode::FormErr;
      }
    }

    vector<const DNSRecord *> cnamesToAdd, nonCnamesToAdd;
    for(const auto & answer : mdp.d_answers) {
      const DNSRecord *dnsRecord = &answer;
      if (dnsRecord->d_place == DNSResourceRecord::AUTHORITY) {
        /* see if it's permitted by policy */
        if (this->d_update_policy_lua != nullptr) {
          if (!this->d_update_policy_lua->updatePolicy(dnsRecord->d_name, QType(dnsRecord->d_type), di.zone.operator const DNSName&(), packet)) {
            g_log<<Logger::Warning<<msgPrefix<<"Refusing update for " << dnsRecord->d_name << "/" << QType(dnsRecord->d_type).toString() << ": Not permitted by policy"<<endl;
            continue;
          } else {
            g_log<<Logger::Debug<<msgPrefix<<"Accepting update for " << dnsRecord->d_name << "/" << QType(dnsRecord->d_type).toString() << ": Permitted by policy"<<endl;
          }
        }

        if (dnsRecord->d_class == QClass::NONE  && dnsRecord->d_type == QType::NS && dnsRecord->d_name == di.zone.operator const DNSName&()) {
          nsRRtoDelete.push_back(dnsRecord);
        }
        else if (dnsRecord->d_class == QClass::IN &&  dnsRecord->d_ttl > 0) {
          if (dnsRecord->d_type == QType::CNAME) {
            cnamesToAdd.push_back(dnsRecord);
          } else {
            nonCnamesToAdd.push_back(dnsRecord);
          }
        }
        else
          changedRecords += performUpdate(msgPrefix, dnsRecord, &di, isPresigned, &narrow, &haveNSEC3, &ns3pr, &updatedSerial);
      }
    }
    for (const auto &rr : cnamesToAdd) {
      DNSResourceRecord rec;
      di.backend->lookup(QType(QType::ANY), rr->d_name, di.id);
      while (di.backend->get(rec)) {
        if (rec.qtype != QType::CNAME && rec.qtype != QType::ENT && rec.qtype != QType::RRSIG) {
          // leave database handle in a consistent state
          while (di.backend->get(rec))
            ;
          g_log<<Logger::Warning<<msgPrefix<<"Refusing update for " << rr->d_name << "/" << QType(rr->d_type).toString() << ": Data other than CNAME exists for the same name"<<endl;
          di.backend->abortTransaction();
          return RCode::Refused;
        }
      }
      changedRecords += performUpdate(msgPrefix, rr, &di, isPresigned, &narrow, &haveNSEC3, &ns3pr, &updatedSerial);
    }
    for (const auto &rr : nonCnamesToAdd) {
      DNSResourceRecord rec;
      di.backend->lookup(QType(QType::CNAME), rr->d_name, di.id);
      while (di.backend->get(rec)) {
        if (rec.qtype == QType::CNAME && rr->d_type != QType::RRSIG) {
          // leave database handle in a consistent state
          while (di.backend->get(rec))
            ;
          g_log<<Logger::Warning<<msgPrefix<<"Refusing update for " << rr->d_name << "/" << QType(rr->d_type).toString() << ": CNAME exists for the same name"<<endl;
          di.backend->abortTransaction();
          return RCode::Refused;
        }
      }
      changedRecords += performUpdate(msgPrefix, rr, &di, isPresigned, &narrow, &haveNSEC3, &ns3pr, &updatedSerial);
    }
    if (nsRRtoDelete.size()) {
      vector<DNSResourceRecord> nsRRInZone;
      DNSResourceRecord rec;
      di.backend->lookup(QType(QType::NS), di.zone.operator const DNSName&(), di.id);
      while (di.backend->get(rec)) {
        nsRRInZone.push_back(rec);
      }
      if (nsRRInZone.size() > nsRRtoDelete.size()) { // only delete if the NS's we delete are less then what we have in the zone (3.4.2.4)
        for (auto& inZone: nsRRInZone) {
          for (auto& rr: nsRRtoDelete) {
            if (inZone.getZoneRepresentation() == (rr)->getContent()->getZoneRepresentation())
              changedRecords += performUpdate(msgPrefix, rr, &di, isPresigned, &narrow, &haveNSEC3, &ns3pr, &updatedSerial);
          }
        }
      }
    }

    // Section 3.6 - Update the SOA serial - outside of performUpdate because we do a SOA update for the complete update message
    if (changedRecords > 0 && !updatedSerial) {
      increaseSerial(msgPrefix, &di, soaEditSetting, haveNSEC3, narrow, &ns3pr);
      changedRecords++;
    }

    if (changedRecords > 0) {
      if (!di.backend->commitTransaction()) {
       g_log<<Logger::Error<<msgPrefix<<"Failed to commit updates!"<<endl;
        return RCode::ServFail;
      }

      S.deposit("dnsupdate-changes", changedRecords);

      d_dk.clearMetaCache(di.zone);
      // Purge the records!
      purgeAuthCaches(di.zone.operator const DNSName&().toString() + "$");

      // Notify secondaries
      if (di.kind == DomainInfo::Primary) {
        vector<string> notify;
        B.getDomainMetadata(packet.qdomainzone, "NOTIFY-DNSUPDATE", notify);
        if (!notify.empty() && notify.front() == "1") {
          Communicator.notifyDomain(di.zone, &B);
        }
      }

      g_log<<Logger::Info<<msgPrefix<<"Update completed, "<<changedRecords<<" changed records committed."<<endl;
    } else {
      //No change, no commit, we perform abort() because some backends might like this more.
      g_log<<Logger::Info<<msgPrefix<<"Update completed, 0 changes, rolling back."<<endl;
      di.backend->abortTransaction();
    }
    return RCode::NoError; //rfc 2136 3.4.2.5
  }
  catch (SSqlException &e) {
    g_log<<Logger::Error<<msgPrefix<<"Caught SSqlException: "<<e.txtReason()<<"; Sending ServFail!"<<endl;
    di.backend->abortTransaction();
    return RCode::ServFail;
  }
  catch (DBException &e) {
    g_log<<Logger::Error<<msgPrefix<<"Caught DBException: "<<e.reason<<"; Sending ServFail!"<<endl;
    di.backend->abortTransaction();
    return RCode::ServFail;
  }
  catch (PDNSException &e) {
    g_log<<Logger::Error<<msgPrefix<<"Caught PDNSException: "<<e.reason<<"; Sending ServFail!"<<endl;
    di.backend->abortTransaction();
    return RCode::ServFail;
  }
  catch(std::exception &e) {
    g_log<<Logger::Error<<msgPrefix<<"Caught std:exception: "<<e.what()<<"; Sending ServFail!"<<endl;
    di.backend->abortTransaction();
    return RCode::ServFail;
  }
  catch (...) {
    g_log<<Logger::Error<<msgPrefix<<"Caught unknown exception when performing update. Sending ServFail!"<<endl;
    di.backend->abortTransaction();
    return RCode::ServFail;
  }
}

void PacketHandler::increaseSerial(const string &msgPrefix, const DomainInfo *di, const string& soaEditSetting,  bool haveNSEC3, bool narrow, const NSEC3PARAMRecordContent *ns3pr) {
  SOAData sd;
  if (!di->backend->getSOA(di->zone, di->id, sd)) {
    throw PDNSException("SOA-Serial update failed because there was no SOA. Wowie.");
  }

  uint32_t oldSerial = sd.serial;

  vector<string> soaEdit2136Setting;
  B.getDomainMetadata(di->zone, "SOA-EDIT-DNSUPDATE", soaEdit2136Setting);
  string soaEdit2136 = "DEFAULT";
  string soaEdit;
  if (!soaEdit2136Setting.empty()) {
    soaEdit2136 = soaEdit2136Setting[0];
    if (pdns_iequals(soaEdit2136, "SOA-EDIT") || pdns_iequals(soaEdit2136,"SOA-EDIT-INCREASE") ){
      if (soaEditSetting.empty()) {
        g_log<<Logger::Error<<msgPrefix<<"Using "<<soaEdit2136<<" for SOA-EDIT-DNSUPDATE increase on DNS update, but SOA-EDIT is not set for domain \""<< di->zone <<"\". Using DEFAULT for SOA-EDIT-DNSUPDATE"<<endl;
        soaEdit2136 = "DEFAULT";
      } else
        soaEdit = soaEditSetting;
    }
  }

  DNSResourceRecord rr;
  if (makeIncreasedSOARecord(sd, soaEdit2136, soaEdit, rr)) {
    di->backend->replaceRRSet(di->id, rr.qname, rr.qtype, vector<DNSResourceRecord>(1, rr));
    g_log << Logger::Notice << msgPrefix << "Increasing SOA serial (" << oldSerial << " -> " << sd.serial << ")" << endl;

    //Correct ordername + auth flag
    DNSName ordername;
    if (haveNSEC3) {
      if (!narrow) {
        ordername = DNSName(toBase32Hex(hashQNameWithSalt(*ns3pr, rr.qname)));
      }
    } else { // NSEC
      ordername = rr.qname.makeRelative(di->zone);
    }
    di->backend->updateDNSSECOrderNameAndAuth(di->id, rr.qname, ordername, true, QType::ANY, haveNSEC3);
  }
}
