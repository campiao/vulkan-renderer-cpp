target("spdlog")
    set_kind("headeronly")
    set_group("Dependencies")

    add_headerfiles("include/**.h")
    add_includedirs("include", {public=true})
    add_linkdirs("lib", {public=true})
    add_syslinks("spdlogd.lib", {public=true})
