#!/usr/bin/make -f
#
#   Copyright information
#
#	Copyright (C) 2010 Jari Aalto
#
#   License
#
#       This program is free software; you can redistribute it and/or modify
#       it under the terms of the GNU General Public License as published by
#       the Free Software Foundation; either version 2 of the License, or
#       (at your option) any later version.
#
#       This program is distributed in the hope that it will be useful,
#       but WITHOUT ANY WARRANTY; without even the implied warranty of
#       MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
#       GNU General Public License for more details.
#
#       You should have received a copy of the GNU General Public License
#       along with this program. If not, see <http://www.gnu.org/licenses/>.

PODCENTER = Lrzip

all: lrunzip.1 lrztar.1

lrunzip.1: lrunzip.1.pod
	podchecker $<
	$(MAKE) -f pod2man.mk PACKAGE=lrunzip PODCENTER=$(PODCENTER) makeman

lrztar.1: lrztar.1.pod
	podchecker $<
	$(MAKE) -f pod2man.mk PACKAGE=lrztar PODCENTER=$(PODCENTER) makeman

# End of file
