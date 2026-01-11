target("vkbootstrap")
  set_kind("static")
  set_group("Dependencies")

  add_files("*.cpp")
  add_headerfiles("*.h")
  add_includedirs(".", {public = true})
  add_includedirs(os.getenv("VULKAN_SDK") .. "/Include", {public = true})
