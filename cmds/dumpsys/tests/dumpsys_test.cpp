/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "../dumpsys.h"

#include <libgen.h>
#include <sys/stat.h>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <android-base/file.h>
#include <android-base/stringprintf.h>
#include <android-base/test_utils.h>
#include <utils/String16.h>
#include <utils/Vector.h>

using namespace android;

using ::testing::_;
using ::testing::Action;
using ::testing::ActionInterface;
using ::testing::DoAll;
using ::testing::EndsWith;
using ::testing::Eq;
using ::testing::HasSubstr;
using ::testing::MakeAction;
using ::testing::Not;
using ::testing::Return;
using ::testing::StartsWith;
using ::testing::StrEq;
using ::testing::Test;
using ::testing::WithArg;
using ::testing::internal::CaptureStderr;
using ::testing::internal::CaptureStdout;
using ::testing::internal::GetCapturedStderr;
using ::testing::internal::GetCapturedStdout;

class ServiceManagerMock : public IServiceManager {
  public:
    MOCK_CONST_METHOD1(getService, sp<IBinder>(const String16&));
    MOCK_CONST_METHOD1(checkService, sp<IBinder>(const String16&));
    MOCK_METHOD3(addService, status_t(const String16&, const sp<IBinder>&, bool));
    MOCK_METHOD0(listServices, Vector<String16>());

  protected:
    MOCK_METHOD0(onAsBinder, IBinder*());
};

class BinderMock : public BBinder {
  public:
    BinderMock() {
    }

    MOCK_METHOD2(dump, status_t(int, const Vector<String16>&));
};

// gmock black magic to provide a WithArg<0>(WriteOnFd(output)) matcher
typedef void WriteOnFdFunction(int);

class WriteOnFdAction : public ActionInterface<WriteOnFdFunction> {
  public:
    explicit WriteOnFdAction(const std::string& output) : output_(output) {
    }
    virtual Result Perform(const ArgumentTuple& args) {
        int fd = ::std::tr1::get<0>(args);
        android::base::WriteStringToFd(output_, fd);
    }

  private:
    std::string output_;
};

// Matcher used to emulate dump() by writing on its file descriptor.
Action<WriteOnFdFunction> WriteOnFd(const std::string& output) {
    return MakeAction(new WriteOnFdAction(output));
}

// Matcher for args using Android's Vector<String16> format
// TODO: move it to some common testing library
MATCHER_P(AndroidElementsAre, expected, "") {
    std::ostringstream errors;
    if (arg.size() != expected.size()) {
        errors << " sizes do not match (expected " << expected.size() << ", got " << arg.size()
               << ")\n";
    }
    int i = 0;
    std::ostringstream actual_stream, expected_stream;
    for (String16 actual : arg) {
        std::string actual_str = String16::std_string(actual);
        std::string expected_str = expected[i];
        actual_stream << "'" << actual_str << "' ";
        expected_stream << "'" << expected_str << "' ";
        if (actual_str != expected_str) {
            errors << " element mismatch at index " << i << "\n";
        }
        i++;
    }

    if (!errors.str().empty()) {
        errors << "\nExpected args: " << expected_stream.str()
               << "\nActual args: " << actual_stream.str();
        *result_listener << errors.str();
        return false;
    }
    return true;
}

// Custom action to sleep for timeout seconds
ACTION_P(Sleep, timeout) {
    sleep(timeout);
}

class DumpsysTest : public Test {
  public:
    DumpsysTest() : sm_(), dump_(&sm_), stdout_(), stderr_() {
    }

    void ExpectListServices(std::vector<std::string> services) {
        Vector<String16> services16;
        for (auto& service : services) {
            services16.add(String16(service.c_str()));
        }
        EXPECT_CALL(sm_, listServices()).WillRepeatedly(Return(services16));
    }

    sp<BinderMock> ExpectCheckService(const char* name, bool running = true) {
        sp<BinderMock> binder_mock;
        if (running) {
            binder_mock = new BinderMock;
        }
        EXPECT_CALL(sm_, checkService(String16(name))).WillRepeatedly(Return(binder_mock));
        return binder_mock;
    }

    void ExpectDump(const char* name, const std::string& output) {
        sp<BinderMock> binder_mock = ExpectCheckService(name);
        EXPECT_CALL(*binder_mock, dump(_, _))
            .WillRepeatedly(DoAll(WithArg<0>(WriteOnFd(output)), Return(0)));
    }

    void ExpectDumpWithArgs(const char* name, std::vector<std::string> args,
                            const std::string& output) {
        sp<BinderMock> binder_mock = ExpectCheckService(name);
        EXPECT_CALL(*binder_mock, dump(_, AndroidElementsAre(args)))
            .WillRepeatedly(DoAll(WithArg<0>(WriteOnFd(output)), Return(0)));
    }

    void ExpectDumpAndHang(const char* name, int timeout_s, const std::string& output) {
        sp<BinderMock> binder_mock = ExpectCheckService(name);
        EXPECT_CALL(*binder_mock, dump(_, _))
            .WillRepeatedly(DoAll(Sleep(timeout_s), WithArg<0>(WriteOnFd(output)), Return(0)));
    }

    void CallMain(const std::vector<std::string>& args, int expected_status = 0) {
        const char* argv[1024] = {"/some/virtual/dir/dumpsys"};
        int argc = (int)args.size() + 1;
        int i = 1;
        for (const std::string& arg : args) {
            argv[i++] = arg.c_str();
        }
        CaptureStdout();
        CaptureStderr();
        int status = dump_.main(argc, const_cast<char**>(argv));
        stdout_ = GetCapturedStdout();
        stderr_ = GetCapturedStderr();
        EXPECT_THAT(status, Eq(expected_status));
    }

    void AssertRunningServices(const std::vector<std::string>& services) {
        std::string expected("Currently running services:\n");
        for (const std::string& service : services) {
            expected.append("  ").append(service).append("\n");
        }
        EXPECT_THAT(stdout_, HasSubstr(expected));
    }

    void AssertOutput(const std::string& expected) {
        EXPECT_THAT(stdout_, StrEq(expected));
    }

    void AssertOutputContains(const std::string& expected) {
        EXPECT_THAT(stdout_, HasSubstr(expected));
    }

    void AssertDumped(const std::string& service, const std::string& dump) {
        EXPECT_THAT(stdout_, HasSubstr("DUMP OF SERVICE " + service + ":\n" + dump));
    }

    // Need functions that returns void to use assertions -
    // https://github.com/google/googletest/blob/master/googletest/docs/AdvancedGuide.md#assertion-placement
    void ReadFileToString(const std::string& path, std::string* content) {
        ASSERT_TRUE(android::base::ReadFileToString(path, content))
            << "could not read contents from " << path;
    }

    void AssertDumpedOnFile(const std::string& path, const std::string& dump) {
        std::string content;
        ReadFileToString(path, &content);
        EXPECT_THAT(content, StrEq(dump));
    }

    void AssertDumpedOnDir(const std::string& dir, const std::string& service,
                           const std::string& dump, bool assert_duration = true) {
        std::string path = dir + "/" + service + ".txt";
        EXPECT_THAT(stdout_, HasSubstr("DUMP OF SERVICE " + service + " ON " + path));

        std::string content;
        ReadFileToString(path, &content);

        if (assert_duration) {
            EXPECT_THAT(content, StartsWith(dump));
            EXPECT_THAT(content, EndsWith(" was the duration\n"));
        } else {
            EXPECT_THAT(content, StrEq(dump));
        }
    }

    void AssertNotDumped(const std::string& dump) {
        EXPECT_THAT(stdout_, Not(HasSubstr(dump)));
    }

    void AssertNotDumpedOnDir(const std::string& dir, const std::string& service) {
        std::string path = dir + "/" + service + ".txt";
        struct stat buffer;
        bool exists = stat(path.c_str(), &buffer) == 0;

        ASSERT_FALSE(exists) << "file " << path << " should not exist";
    }

    void AssertStopped(const std::string& service) {
        EXPECT_THAT(stderr_, HasSubstr("Can't find service: " + service + "\n"));
    }

    ServiceManagerMock sm_;
    Dumpsys dump_;

  private:
    std::string stdout_, stderr_;
};

// Tests 'dumpsys -l' when all services are running
TEST_F(DumpsysTest, ListAllServices) {
    ExpectListServices({"Locksmith", "Valet"});
    ExpectCheckService("Locksmith");
    ExpectCheckService("Valet");

    CallMain({"-l"});

    AssertRunningServices({"Locksmith", "Valet"});
}

// Tests 'dumpsys -l' when a service is not running
TEST_F(DumpsysTest, ListRunningServices) {
    ExpectListServices({"Locksmith", "Valet"});
    ExpectCheckService("Locksmith");
    ExpectCheckService("Valet", false);

    CallMain({"-l"});

    AssertRunningServices({"Locksmith"});
    AssertNotDumped({"Valet"});
}

// Tests 'dumpsys service_name' on a service is running
TEST_F(DumpsysTest, DumpRunningService) {
    ExpectDump("Valet", "Here's your car");

    CallMain({"Valet"});

    AssertOutput("Here's your car");
}

// Tests 'dumpsys -t 1 service_name' on a service that times out after 2s
TEST_F(DumpsysTest, DumpRunningServiceTimeout) {
    ExpectDumpAndHang("Valet", 2, "Here's your car");

    CallMain({"-t", "1", "Valet"});

    AssertOutputContains("SERVICE 'Valet' DUMP TIMEOUT (1s) EXPIRED");
    AssertNotDumped("Here's your car");

    // Must wait so binder mock is deleted, otherwise test will fail with a leaked object
    sleep(1);
}

// Tests 'dumpsys service_name Y U NO HAVE ARGS' on a service that is running
TEST_F(DumpsysTest, DumpWithArgsRunningService) {
    ExpectDumpWithArgs("SERVICE", {"Y", "U", "NO", "HANDLE", "ARGS"}, "I DO!");

    CallMain({"SERVICE", "Y", "U", "NO", "HANDLE", "ARGS"});

    AssertOutput("I DO!");
}

// Tests 'dumpsys' with no arguments
TEST_F(DumpsysTest, DumpMultipleServices) {
    ExpectListServices({"running1", "stopped2", "running3"});
    ExpectDump("running1", "dump1");
    ExpectCheckService("stopped2", false);
    ExpectDump("running3", "dump3");

    CallMain({});

    AssertRunningServices({"running1", "running3"});
    AssertDumped("running1", "dump1");
    AssertStopped("stopped2");
    AssertDumped("running3", "dump3");
}

// Tests 'dumpsys --skip skipped3 skipped5', which should skip these services
TEST_F(DumpsysTest, DumpWithSkip) {
    ExpectListServices({"running1", "stopped2", "skipped3", "running4", "skipped5"});
    ExpectDump("running1", "dump1");
    ExpectCheckService("stopped2", false);
    ExpectDump("skipped3", "dump3");
    ExpectDump("running4", "dump4");
    ExpectDump("skipped5", "dump5");

    CallMain({"--skip", "skipped3", "skipped5"});

    AssertRunningServices({"running1", "running4", "skipped3 (skipped)", "skipped5 (skipped)"});
    AssertDumped("running1", "dump1");
    AssertDumped("running4", "dump4");
    AssertStopped("stopped2");
    AssertNotDumped("dump3");
    AssertNotDumped("dump5");
}

// Tests 'dumpsys -d DIRECTORY', which dumps all services in the given directory
TEST_F(DumpsysTest, DumpOnDirectoryMultiple) {
    TemporaryDir root;
    std::string dir = root.path;

    ExpectListServices({"running1", "stopped2", "running3"});
    ExpectDump("running1", "dump1");
    ExpectCheckService("stopped2", false);
    ExpectDump("running3", "dump3");

    CallMain({"-d", root.path});

    AssertRunningServices({"running1", "running3"});

    AssertDumpedOnDir(root.path, "running1", "dump1");
    AssertDumpedOnDir(root.path, "running3", "dump3");

    AssertNotDumpedOnDir(root.path, "stopped2");
    AssertStopped("stopped2");
}

// Tests 'dumpsys -d DIRECTORY service_name', which is not valid
TEST_F(DumpsysTest, DumpOnDirectorySingleService) {
    TemporaryDir root;
    CallMain({"-d", root.path, "Valet"}, 1);
    AssertNotDumpedOnDir(root.path, "Valet");
}

// Tests 'dumpsys -d DIRECTORY --skip skipped3 skipped5', which should skip these services
TEST_F(DumpsysTest, DumpOnDirectorySkips) {
    TemporaryDir root;

    ExpectListServices({"running1", "stopped2", "skipped3", "running4", "skipped5"});
    ExpectDump("running1", "dump1");
    ExpectCheckService("stopped2", false);
    ExpectDump("skipped3", "dump3");
    ExpectDump("running4", "dump4");
    ExpectDump("skipped5", "dump5");

    CallMain({"-d", root.path, "--skip", "skipped3", "skipped5"});

    AssertRunningServices({"running1", "running4", "skipped3 (skipped)", "skipped5 (skipped)"});
    AssertDumpedOnDir(root.path, "running1", "dump1");
    AssertDumpedOnDir(root.path, "running4", "dump4");

    AssertNotDumpedOnDir(root.path, "stopped2");
    AssertStopped("stopped2");

    AssertNotDumpedOnDir(root.path, "skipped3");
    AssertNotDumpedOnDir(root.path, "skipped5");
}

// Tests 'dumpsys -o FILE service_name', which dumps just one service using a custom file name
TEST_F(DumpsysTest, DumpOnFile) {
    TemporaryDir root;
    std::string path = android::base::StringPrintf("%s/Valet.txt", root.path);

    ExpectDump("Valet", "Here's your car");

    CallMain({"-o", path, "Valet"});

    AssertDumpedOnFile(path, "Here's your car");
}

// Tests 'dumpsys -o FILE', which is not valid
TEST_F(DumpsysTest, DumpOnFileMultipleServices) {
    TemporaryDir root;
    std::string path = android::base::StringPrintf("%s/Y_U_NO_FAIL.txt", root.path);

    CallMain({"-o", path}, 1);
}
// TODO(felipeal): test file errors (dir not found, cannot write dir, file not found, etc...)
