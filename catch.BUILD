licenses(["notice"]) # boost license

CATCH_PATH = "Catch-1.2.1/include"

cc_library(
    visibility = ["//visibility:public"],
    name = "main",
    hdrs = [ CATCH_PATH + "/catch.hpp" ],
    srcs = glob([
        CATCH_PATH + "/**/.hpp",
        CATCH_PATH + "/**/.h",
    ]),
    includes = [ CATCH_PATH ],
)
