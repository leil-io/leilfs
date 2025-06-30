if is_program_installed pylint && pylint --version | grep -q "Python 3" ; then
	pylintexec="pylint"
elif is_program_installed pylint3 ; then
	pylintexec="pylint3"
else
	test_fail "pylint is not installed"
fi

# Get paths to sfs.cgi, chart.cgi and the CGI server
mapfile -t files < <(ls -1 "${SAUNAFS_ROOT}/share/sfscgi/"*.cgi "${SAUNAFS_ROOT}/sbin/saunafs-cgiserver")

# Validate all found files using pylint
expect_empty "$(${pylintexec} -E --ignored-modules=cgi,cgitb "${files[@]}" || true)"
