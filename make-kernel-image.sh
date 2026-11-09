#!/bin/sh
# (Re)create the glos-kernel:ready Docker image the module and helper builds
# use (build-module.sh, fbfix/build.sh, helper/build.sh, assemble-glos.sh):
# the stock goodluckOS kernel tree with `make modules` done
# (kernel/Dockerfile.modules). About 3 GB; when done with the driver:
#   docker image rm glos-kernel:ready && docker builder prune -f
set -eu
cd "$(dirname "$0")"
docker buildx build -f kernel/Dockerfile.modules -t glos-kernel:ready --load kernel
