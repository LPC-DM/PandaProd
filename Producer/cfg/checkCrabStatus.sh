#Usage: ./checkCrabStatus {crab working directory}

WORKING_DIR=$1

find  ${WORKING_DIR} -mindepth 1 -maxdepth 1 -type d -exec bash -c 'crab status --verboseErrors {}' {} \;
