Installing PowerDNS
===================

Installation of the PowerDNS Authoritative server on UNIX systems can be
done in several ways:

-  Binary packages provided by your distribution
-  Binary packages provided by PowerDNS on
   `repo.powerdns.com <https://repo.powerdns.com>`__

Binary Packages
---------------

Debian-based Systems
~~~~~~~~~~~~~~~~~~~~

PowerDNS Authoritative Server is available through the `apt <https://packages.debian.org/pdns-server>`__ system.
Your distribution likely ships a package, but we recommend getting more recent packages from `the PowerDNS repositories <https://repo.powerdns.com>`__.
Please see the instructions on the repo site and then come back here!

.. code-block:: shell

    $ sudo apt-get install pdns-server

Debian splits the backends into `several different
packages <https://packages.debian.org/pdns-backend>`__, install the
required backend as follows:

.. code-block:: shell

    $ sudo apt-get install pdns-backend-$backend

Red Hat-based Systems
~~~~~~~~~~~~~~~~~~~~~

On Red Hat based systems there are 2 options to install PowerDNS, from
`EPEL <https://fedoraproject.org/wiki/EPEL>`__, 
or from `the PowerDNS repositories <https://repo.powerdns.com>`__:

Add either to your list of repositories and install PowerDNS by issuing:

.. code-block:: shell

    $ sudo yum install pdns

The different backends can be installed using

.. code-block:: shell

    $ sudo yum install pdns-backend-$backend

Note that for some of those package sources, the bind backend is shipped as part of the base ``pdns`` package, and there is no separate ``pdns-backend-bind`` package.

FreeBSD
~~~~~~~

PowerDNS Authoritative Server is available through the
`ports <https://www.freshports.org/dns/powerdns/>`__ system:

For the package:

.. code-block:: shell

    $ sudo pkg install dns/powerdns

To have your system build the port:

.. code-block:: shell

    cd /usr/ports/dns/powerdns/ && make install clean

Mac OS X
~~~~~~~~

PowerDNS Authoritative Server is available through Homebrew:

.. code-block:: shell

    $ brew install pdns

After installation
------------------

Once installed, try :doc:`guides/basic-database` using SQLite 3 or start :doc:`migrating <migration>` your data.
