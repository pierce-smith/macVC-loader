-- macVC Plugin Loader installer: double-click, or drop macVC.app onto it.
on run
	set appPath to my findMacVC()
	if appPath is "" then
		set appPath to POSIX path of (choose file of type {"app"} with prompt "Select macVC.app" default location (path to desktop folder))
	end if
	my manage(appPath)
end run

on open droppedItems
	repeat with anItem in droppedItems
		my manage(POSIX path of anItem)
	end repeat
end open

on findMacVC()
	repeat with p in {(POSIX path of (path to desktop folder)) & "macVC.app", "/Applications/macVC.app", (POSIX path of (path to home folder)) & "Applications/macVC.app"}
		try
			do shell script "test -d " & quoted form of p
			return p as text
		end try
	end repeat
	return ""
end findMacVC

on manage(appPath)
	set here to POSIX path of (path to me)
	set root to do shell script "dirname " & quoted form of here
	set status to do shell script "cd " & quoted form of root & " && v=$(/usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' " & quoted form of (appPath & "/Contents/Info.plist") & " 2>/dev/null); if build/mvc-patch check " & quoted form of (appPath & "/Contents/MacOS/macVC") & " @executable_path/../Frameworks/macVCLoader.dylib; then echo \"macVC $v - plugin loader is installed.\"; else echo \"macVC $v - plugin loader is NOT installed (new install, or macVC was updated).\"; fi"
	set choice to button returned of (display dialog status & return & return & appPath buttons {"Uninstall", "Cancel", "Install / Update"} default button 3 cancel button 2 with title "macVC Plugin Loader")
	if choice is "Install / Update" then
		set script_ to "install.sh"
	else
		set script_ to "uninstall.sh"
	end if
	try
		set out to do shell script "cd " & quoted form of root & " && ./" & script_ & " " & quoted form of appPath & " 2>&1"
		display dialog out buttons {"OK"} default button 1 with title "macVC Plugin Loader"
	on error errText
		display dialog errText buttons {"OK"} default button 1 with title "macVC Plugin Loader" with icon caution
	end try
end manage
