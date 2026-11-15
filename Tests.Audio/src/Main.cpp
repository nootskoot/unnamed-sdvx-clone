#include "stdafx.h"
#include "SmokeAudio.hpp"

#include <Shared/Path.hpp>
#include <Tests/TestManager.hpp>

#include <typeinfo>

static void ListTests()
{
	TestManager &testManager = TestManager::Get();
	Vector<String> testNames = testManager.GetAvailableTests();
	Logf("Available Tests:", Logger::Severity::Info, typeid(testManager).name());
	for (size_t i = 0; i < testNames.size(); i++)
	{
		Logf(" %s", Logger::Severity::Info, testNames[i]);
	}
}

int main(int argc, char **argv)
{
	Vector<String> cmdLine = Path::SplitCommandLine(argc, argv);

	// No arguments: render a block through every DSP through the stub AudioBase.
	// This is the harness self-check, not a chart test.
	//
	// SplitCommandLine keeps argv[0] (Shared/src/Path.cpp:120-131), so "no
	// arguments" arrives as a single entry holding the executable path, never as
	// an empty vector. Testing empty() here made every bare run fall through to
	// the test-name lookup and print 'Test "./Tests.Audio" not found'.
	if (cmdLine.size() <= 1)
	{
		return RunAudioSmoke();
	}

	String execName = cmdLine.back();
	TestManager &testManager = TestManager::Get();
	Vector<String> testNames = testManager.GetAvailableTests();
	if (!testNames.Contains(execName))
	{
		Logf("Test \"%s\" not found", Logger::Severity::Error, execName);
		ListTests();
		return 1;
	}

	return testManager.Run(execName);
}
