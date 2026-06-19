#pragma once

#define DORUN_VERSION_MAJOR 0
#define DORUN_VERSION_MINOR 2
#define DORUN_VERSION_PATCH 4
#define DORUN_VERSION_BUILD 38

#define DORUN_VERSION_STR_IMPL(major, minor, patch, build) #major "." #minor "." #patch "." #build
#define DORUN_VERSION_STR(major, minor, patch, build) DORUN_VERSION_STR_IMPL(major, minor, patch, build)
#define DORUN_VERSION_STRING DORUN_VERSION_STR(DORUN_VERSION_MAJOR, DORUN_VERSION_MINOR, DORUN_VERSION_PATCH, DORUN_VERSION_BUILD)
