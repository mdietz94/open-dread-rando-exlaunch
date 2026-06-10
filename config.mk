#----------------------------- User configuration -----------------------------

# Common settings
#------------------------

# How you're loading your module. Used to determine how to find the target module. (AsRtld/Module/Kip)
LOAD_KIND := Module

# Program you're targetting. Used to determine where to deploy your files.
PROGRAM_ID := 010093801237c000

# Optional path to copy the final ELF to, for convenience.
ELF_EXTRACT :=

# Python command to use. Must be Python 3.4+.
PYTHON := python3

# JSON to use to make .npdm
NPDM_JSON := qlaunch.json

# Bridge fork: MOD_VERSION lands in the HELLO envelope; passed by
# apworld/dread/_setup/build.py via the environment, with a safe default for
# direct `make` invocations. The Switch's /24 sweep seed is NOT baked — it is
# read at runtime from rom:/ap_config.json (see source/program/discovery.cpp),
# so one build targets any LAN.
MOD_VERSION ?= dread-bridge-dev

# Additional C/C++ flags to use.
C_FLAGS := -DMOD_VERSION_STRING=\"$(MOD_VERSION)\"
CXX_FLAGS := -DMOD_VERSION_STRING=\"$(MOD_VERSION)\"

# AsRtld settings
#------------------------

# Path to the SD card. Used to mount and deploy files on SD, likely with hekate UMS.
MOUNT_PATH := /mnt/k

# Module settings
#------------------------

# Settings for deploying over FTP. Used by the deploy-ftp.py script.
FTP_IP := 192.168.0.235
FTP_PORT := 5000
FTP_USERNAME := anonymous
FTP_PASSWORD :=

# Settings for deploying to Ryu. Used by the deploy-ryu.sh script.
RYU_PATH := /c/Users/maxwe/AppData/Roaming/Ryujinx
