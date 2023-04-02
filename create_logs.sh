#! /bin/sh

#
# Script for empty logs creation.
# Adjust variables by your requirements
# in according with adapter_gzip.h
#
# Y.Voinov (C) 2016-2019
#

LOG_PREFIX="/var/log"
SQUID_USER="squid"
SQUID_GROUP="squid"

ERR_LOG_NAME="ecap_gzip_err.log"
UT_LOG_NAME="ecap_gzip_comp.log"

# Create error log
touch $LOG_PREFIX/$ERR_LOG_NAME
# Change permissions
chown $SQUID_USER:$SQUID_GROUP $LOG_PREFIX/$ERR_LOG_NAME

# Create utilization log
touch $LOG_PREFIX/$UT_LOG_NAME
# Change permissions
chown $SQUID_USER:$SQUID_GROUP $LOG_PREFIX/$UT_LOG_NAME
