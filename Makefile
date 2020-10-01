# Makefile for a HTTP Proxy Server
#
#    Copyright 2020 - Eric Park
#
# Useful sample program targets:
#
#    httpproxy - a basic HTTP proxy server
#
#  Maintenance targets:
#
#    Make sure these clean up and build your code too
#
#    clean       - clean out all compiled object and executable files
#    all         - (default target) make sure everything's compiled
#

# Preliminary
GCC = gcc
#GCCFLAGS =
# INCLUDES =

all: httpproxy

#
# Build the httpproxy
#
httpproxy:
	$(GCC) main.c

#
# Delete all compiled code in preparation
# for forcing complete rebuild#
clean:
	rm -f httpproxy