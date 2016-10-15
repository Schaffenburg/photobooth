#! /usr/bin/python2
# -*- coding: utf-8 -*-

import sys

prints_list = []

CUPSLOGFILE = "/var/log/cups/page_log"
f = open(CUPSLOGFILE, 'r')
for line in f.readlines():

#for line in sys.stdin:
  cols = line.split(' ')
  rawdatetime = cols[3][1:]
  copies = int(cols[5])
  job = cols[10]
  #print "rawdatetime", rawdatetime, "copies", copies, "job", job
  if copies == 1:
    prints_list.append((rawdatetime, copies, job))
  elif prints_list and prints_list[-1][2] == job:
    prints_list[-1] = (rawdatetime, copies, job)

MAX_COPIES = 6
copies_statistics = {}
for x in range(1, MAX_COPIES):
  copies_statistics[x] = 0

total = 0
jobs = 0

for date, copies, job in prints_list:
  print date, copies, job
  copies_statistics[copies] += 1
  total += copies
  jobs += 1

word = "Abzug"

for x, copies in copies_statistics.iteritems():
  print str(copies).rjust(4), "x\t", x, word
  word = "Abzüge"

print "----------------------"
print str(total).rjust(4), "\tAbzüge gesamt"
print str(jobs).rjust(4), "\tJobs"


