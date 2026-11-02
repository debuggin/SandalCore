# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
# GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
# ============================================================================
#  libaocproto: unified C++ protobuf & gRPC internal service stubs (CPP-02)
# ============================================================================

set(AOC_PROTO_DIR "${PROJECT_SOURCE_DIR}/proto/internal")
set(AOC_PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${AOC_PROTO_GEN_DIR}")

set(AOC_PROTOS
    aoc/auth/v1/auth.proto
    aoc/character/v1/character.proto
    aoc/citynode/v1/citynode.proto
    aoc/gateway/v1/gateway.proto
    aoc/mesh/v1/mesh.proto
    aoc/quest/v1/quest.proto
    aoc/worldadmin/v1/worldadmin.proto
)

set(AOC_PROTO_SRCS "")
foreach(rel ${AOC_PROTOS})
    string(REGEX REPLACE "[.]proto$" "" base "${rel}")
    set(outs
        "${AOC_PROTO_GEN_DIR}/${base}.pb.cc"
        "${AOC_PROTO_GEN_DIR}/${base}.pb.h"
        "${AOC_PROTO_GEN_DIR}/${base}.grpc.pb.cc"
        "${AOC_PROTO_GEN_DIR}/${base}.grpc.pb.h"
    )
    add_custom_command(
        OUTPUT ${outs}
        COMMAND ${PROTOC}
            "--proto_path=${AOC_PROTO_DIR}"
            "--cpp_out=${AOC_PROTO_GEN_DIR}"
            "--grpc_out=${AOC_PROTO_GEN_DIR}"
            "--plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}"
            "${AOC_PROTO_DIR}/${rel}"
        DEPENDS "${AOC_PROTO_DIR}/${rel}"
        COMMENT "Generating protobuf/gRPC stubs for ${rel}"
    )
    list(APPEND AOC_PROTO_SRCS ${outs})
endforeach()

set_source_files_properties(${AOC_PROTO_SRCS} PROPERTIES GENERATED TRUE)

add_library(aocproto STATIC ${AOC_PROTO_SRCS})
add_library(world_protos ALIAS aocproto)

target_include_directories(aocproto PUBLIC "${AOC_PROTO_GEN_DIR}")
target_link_libraries(aocproto PUBLIC protobuf::libprotobuf gRPC::grpc++)

if(MSVC)
    target_compile_options(aocproto PRIVATE /W0)
else()
    target_compile_options(aocproto PRIVATE -w)
endif()

set_target_properties(aocproto PROPERTIES FOLDER "Libraries")
