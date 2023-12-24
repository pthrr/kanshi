scdoc = dependency(
	'scdoc',
	version: '>=1.9.2',
	native: true,
	required: get_option('man-pages'),
)
if not scdoc.found()
	subdir_done()
endif

scdoc_path = scdoc.get_variable('scdoc')
mandir = get_option('mandir')
man_files = [
	'kanshi.1.scd',
	'kanshi.5.scd',
]
if varlink.found()
	man_files += 'kanshictl.1.scd'
endif
foreach filename : man_files
	topic = filename.split('.')[-3].split('/')[-1]
	section = filename.split('.')[-2]
	output = '@0@.@1@'.format(topic, section)

	custom_target(
		output,
		input: filename,
		output: output,
		command: scdoc_path,
		capture: true,
		feed: true,
		install: true,
		install_dir: '@0@/man@1@'.format(mandir, section)
	)
endforeach
