#
# Copyright 2009-2013 SPARTA, Inc.  All rights reserved.
# See the COPYING file included with the DNSSEC-Tools package for details.
#
# Makefile for building demo data for the basic rollerd demo.
#

SUBDIRS =	rollerd-basic		\
		rollerd-manyzones	\
		rollerd-manyzones-fast	\
		rollerd-split-view	\
		rollerd-subdirs		\
		rollerd-vastzones	\
		rollerd-zonegroups

default:	clean

all:
	@for i in $(SUBDIRS);		\
	do				\
		echo ;			\
		echo Building $$i:;	\
		cd $$i;			\
		make $(MAKE_ARGS) $@;	\
		cd ..;			\
	done

clean:
	@for i in $(SUBDIRS);		\
	do				\
		echo ;			\
		echo Cleaning $$i:;	\
		cd $$i;			\
		make $(MAKE_ARGS) $@;	\
		cd ..;			\
	done
