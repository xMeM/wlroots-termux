wlr_files += files(
	'backend.c',
	'output.c',
	'input.c',
	'allocator.c',
)

wlr_deps += cc.find_library('termuxgui')
wlr_deps += cc.find_library('android')

add_project_arguments(['-target', 'aarch64-linux-android33'], language: 'c')
