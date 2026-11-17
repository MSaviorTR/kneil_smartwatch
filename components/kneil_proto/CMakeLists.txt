# KNEIL live protocol (copied, do not edit)
#
# Source: KNEIL firmware repository, protocol/kneil_proto.h and protocol/kneil_proto.c,
# main @ 718301d ("Merge agent/E2: model V4 (library 2.0), BLE protocol v2, PC app v2").
# The files are byte-identical to that commit (protocol v2): the watch must decode exactly like the
# KNEIL bridge and the PC app, so do not change them here - update the KNEIL repository and copy again.

idf_component_register(
    SRCS kneil_proto.c
    INCLUDE_DIRS "."
)
