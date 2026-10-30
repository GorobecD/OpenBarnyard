project "BYCoop"
	kind "SharedLib"
	language "C++"
	staticruntime "off"
	
	pchheader "pch.h"
	pchsource "Source/pch.cpp"

	links
	{
		"Toshi",
		"BYardSDK",
		"BYModCore"
	}
	
	libdirs
	{
		"%{LibDir.fmod}",
		"%{LibDir.bink}",
		"%{LibDir.dx8}",
		"%{LibDir.detours}",
	}

	files
	{
		"Source/**.h",
		"Source/**.cpp",
	}

	-- Network layer is engine-agnostic and unit tested outside the game (Tests/), no PCH
	filter "files:Source/Net/**.cpp"
		flags { "NoPCH" }
	filter {}

	links { "ws2_32" }

	-- Cyrillic menu texts are UTF-8 in the sources
	buildoptions { "/utf-8" }

	includedirs
	{
		"Source",
		"%{IncludeDir.byardsdk}",
		"%{IncludeDir.modcore}",
		"%{IncludeDir.detours}"
	}

	defines
	{
		"TOSHI_SDK",
		"TOSHI_MODLOADER_CLIENT"
	}
	
	-- Modloader specific
	debugdir ("%{wks.location}/../Game")
	debugcommand ("%{wks.location}/../Game/BYardModLoader.exe")
	
	postbuildcommands
	{
		"{COPYDIR} \"%{wks.location}bin/" .. outputdir .. "/%{prj.name}/\" %{wks.location}../Game/Mods/",
	}
