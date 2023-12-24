project(
	'kanshi',
	'c',
	version: '1.4.0',
	license: 'MIT',
	meson_version: '>=0.59.0',
	default_options: [
		'c_std=c99',
		'warning_level=3',
		'werror=true',
	],
)

cc = meson.get_compiler('c')

add_project_arguments(cc.get_supported_arguments([
	'-Wundef',
	'-Wlogical-op',
	'-Wmissing-include-dirs',
	'-Wold-style-definition',
	'-Wpointer-arith',
	'-Winit-self',
	'-Wfloat-equal',
	'-Wstrict-prototypes',
	'-Wredundant-decls',
	'-Wimplicit-fallthrough=2',
	'-Wendif-labels',
	'-Wstrict-aliasing=2',
	'-Woverflow',
	'-Wformat=2',

	'-Wno-missing-braces',
	'-Wno-missing-field-initializers',
	'-Wno-unused-parameter',
]), language: 'c')

wayland_client = dependency('wayland-client')
varlink = dependency('libvarlink', required: get_option('ipc'))

add_project_arguments([
	'-DKANSHI_VERSION="@0@"'.format(meson.project_version()),
	'-DKANSHI_HAS_VARLINK=@0@'.format(varlink.found().to_int()),
], language: 'c')

subdir('protocol')

kanshi_deps = [
	wayland_client,
	client_protos,
]

kanshi_srcs = [
	'event-loop.c',
	'main.c',
	'parser.c',
	'ipc-addr.c',
]

if varlink.found()
	kanshi_deps += varlink
	kanshi_srcs += ['ipc.c', 'ipc-addr.c']
endif

executable(
	meson.project_name(),
	kanshi_srcs,
	include_directories: 'include',
	dependencies: kanshi_deps,
	install: true,
)

if varlink.found()
	executable(
		meson.project_name() + 'ctl',
		files(
			'ctl.c',
			'ipc-addr.c',
		),
		include_directories: 'include',
		dependencies: [varlink],
		install: true,
	)
endif

subdir('doc')

summary({
	'Man pages': scdoc.found(),
	'IPC': varlink.found(),
}, bool_yn: true)
