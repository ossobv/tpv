tpv - the Text Pipe Viewer
==========================

*tpv* is a filter like *pv* (Pipe Viewer) monitoring the progress of data
through a pipeline. In constrast to *pv* it is designed for text transfer,
giving you some visible feedback of the progress.

And if the destination end aborts unexpectedly (SIGPIPE), you'll also
get some surrounding data, so you have some feel of where in the data
the failure occurred.

Common use case:

.. code-block:: console

    $ bzcat BIG_FILE.sql.bz2 | tpv | mysql my_db
    ...
    tpv[00879d70+001<<15] LOCK TABLES `my_table` WRITE;
    tpv[00879d8e+001<<15] /*!40000 ALTER TABLE `my_table` DISABLE KEYS */;
    tpv[00879dbf+001<<15] INSERT INTO `my_table` VALUES (2,'2009-04-28 16:22:03',...
    tpv: 7208960 (0x6e0000) bytes, 3.4 MiB/s

*tpv* will show the current write position and the write speed. And
every second it will show some context.

In case it stalls, you might see something like this:

.. code-block:: console

    $ bzcat BIG_FILE.sql.bz2 | tpv | mysql my_db
    ...
    tpv[006661d1+01f<<10] INSERT INTO `my_table` VALUES (616184,25237,2),...
    tpv[00757763+01f<<10] INSERT INTO `my_table` VALUES (771588,44447,4),...
    tpv[006661d1+01f<<10] INSERT INTO `my_table` VALUES (616184,25237,2),...
    tpv[00757763+01f<<10] INSERT INTO `my_table` VALUES (771588,44447,4),...
    tpv[006661d1+01f<<10] INSERT INTO `my_table` VALUES (616184,25237,2),...
    tpv[00757763+01f<<10] INSERT INTO `my_table` VALUES (771588,44447,4),...
    tpv: 5963776 (0x5b0000) bytes, 415.9 KiB/s

*Same two queries over again? And a decreasing speed? MySQL "waiting
for metadata lock"?*

And in case the connection write end breaks, might see something like this:

.. code-block:: console

    $ bzcat BIG_FILE.sql.bz2 | tpv | mysql my_db
    ...
    ERROR 1064 (42000) at line 122: You have an error in your SQL
      syntax; check the manual that corresponds to your MySQL server version
      for the right syntax to use near ')'
    ...
    tpv[00760000+001<<15] ...0,0,0,0,NULL,132,NULL),(826,'2010-06-25 14:03:23',...
    tpv: 7733248 (0x760000) bytes, 4.7 MiB/s
    tpv: write/splice error: Broken pipe

*Then you'll know that the error was in the buffer just before or after 0x760000.*

----

Test invocations:

.. code-block:: shell

    cat BIG_FILE BIG_FILE | tpv |
      python3 -c 'import sys;sys.stdin.buffer.read(950352912 + 65535 + 3)'

.. code-block:: shell

    ( n=0; while :; do echo $n; sleep 0.001; n=$((n+1));
      test $n -gt 1000 && break; done ) | tpv >/dev/null

----

TODO:

- keep write buffer around for read() case;

- write buffer output to file on abort(), with byte markers;

- --help, --size for ETA.
