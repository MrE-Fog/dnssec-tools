#
# Copyright 2007-2008 SPARTA, Inc.  All rights reserved.
# See the COPYING file included with the DNSSEC-Tools package for details.
#
# Makefile for building demo data for the basic rollerd demo.
#

DIRDUMMY=	dir-dummy
DIREXAMPLE=	dir-example

basic:		short

basic-rollerd:	basic

basic-short:	short

basic-same:	same

basic-long:	long

new-files:
	mkdir ${DIRDUMMY}
	mkdir ${DIREXAMPLE}

	cp save-demo.rollrec demo.rollrec
	cp save-db.cache db.cache
	cp save-db.cache ${DIRDUMMY}/db.cache
	cp save-db.cache ${DIREXAMPLE}/db.cache
	cp save-dummy.com ${DIRDUMMY}/dummy.com
	cp save-example.com ${DIREXAMPLE}/example.com
	cp save-test.com test.com

	phaser demo.rollrec

zones-short:
	cd ${DIRDUMMY}   ; zonesigner -zsklife 15 -e +25 -genkeys dummy.com
	cd ${DIREXAMPLE} ; zonesigner -zsklife 60 -genkeys example.com
	zonesigner -zsklife 30 -genkeys test.com

zones-same:
	zonesigner -zsklife 60 -genkeys dummy.com
	zonesigner -zsklife 60 -genkeys example.com
	zonesigner -zsklife 60 -genkeys test.com

zones-long:
	zonesigner -zsklife 600 -e +25 -genkeys dummy.com
	zonesigner -zsklife 7200 -genkeys example.com
	zonesigner -zsklife 3600 -genkeys test.com


long:	clean zones-long new-files

short:	clean new-files zones-short

same:	clean zones-same new-files

clean:
	rm -f K* db.* dsset-* keyset-* *.krf *.signed
	rm -f dummy.com example.com test.com db.cache demo.rollrec
	rm -fr ${DIRDUMMY} ${DIREXAMPLE}
	@echo NOT deleting log.demo