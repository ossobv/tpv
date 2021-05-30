textpv
======

Use case:

.. code-block:: console

    $ ./textpv < BIG_SQL_FILE.sql | mysql testdata
    textpv: 21233664+ (0x1440000) bytes written, 21495808+ (0x1480000) bytes read (delta 0x40000), curbuf 0x2b000, 2.9 MiB/s
    ERROR 2013 (HY000) at line 752: Lost connection to MySQL server during query
    textpv: > ... -10-08 21:25:43','2015-10-08 21:25:43',3707,'00:00:00','24:00:00','2015- ...
    textpv: write error: Broken pipe

*Now we know that things went wrong in the vicinity of that line. This
is particularly relevant if the input data is streamed as well, as you
have no way of knowing what "line 752" refers to.*

Tests:

.. code-block:: shell

    cat BIG_FILE BIG_FILE | ./textpv |
      python3 -c 'import sys;sys.stdin.buffer.read(950352912 + 65535 + 3)' )

.. code-block:: shell

    ( n=0; while :; do echo $n; sleep 0.001; n=$((n+1));
      test $n -gt 1000 && break; done ) | ./textpv >/dev/null


