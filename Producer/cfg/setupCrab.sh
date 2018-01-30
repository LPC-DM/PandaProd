#!/bin/bash

eval `scramv1 runtime -sh`
source /cvmfs/cms.cern.ch/crab3/crab_standalone.sh
#source standalone_crab.sh
#source crab.sh
crab --version
voms-proxy-init --voms cms --valid 168:00
voms-proxy-info --all
