/*
 * Copyright (C) 2017 The Android Open Source Project
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

#define LOG_TAG "lshal"
#include <android-base/logging.h>

#include "Lshal.h"

#include <set>
#include <string>

#include <hidl/ServiceManagement.h>
#include <utils/NativeHandle.h>

#include "DebugCommand.h"
#include "ListCommand.h"
#include "PipeRelay.h"

namespace android {
namespace lshal {

using ::android::hidl::manager::V1_0::IServiceManager;

Lshal::Lshal()
    : mOut(std::cout), mErr(std::cerr),
      mServiceManager(::android::hardware::defaultServiceManager()),
      mPassthroughManager(::android::hardware::getPassthroughServiceManager()) {
}

Lshal::Lshal(std::ostream &out, std::ostream &err,
            sp<hidl::manager::V1_0::IServiceManager> serviceManager,
            sp<hidl::manager::V1_0::IServiceManager> passthroughManager)
    : mOut(out), mErr(err),
      mServiceManager(serviceManager),
      mPassthroughManager(passthroughManager) {

}

void Lshal::usage() {
    static const std::string helpSummary =
            "lshal: List and debug HALs.\n"
            "\n"
            "commands:\n"
            "    help            Print help message\n"
            "    list            list HALs\n"
            "    debug           debug a specified HAL\n"
            "\n"
            "If no command is specified, `list` is the default.\n";

    err() << helpSummary << "\n";
    selectCommand("list")->usage();
    err() << "\n";
    selectCommand("debug")->usage();
    err() << "\n";
    selectCommand("help")->usage();
}

static hardware::hidl_vec<hardware::hidl_string> convert(const std::vector<std::string> &v) {
    hardware::hidl_vec<hardware::hidl_string> hv;
    hv.resize(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        hv[i].setToExternal(v[i].c_str(), v[i].size());
    }
    return hv;
}

Status Lshal::emitDebugInfo(
        const std::string &interfaceName,
        const std::string &instanceName,
        const std::vector<std::string> &options,
        std::ostream &out,
        NullableOStream<std::ostream> err) const {
    using android::hidl::base::V1_0::IBase;

    hardware::Return<sp<IBase>> retBase = serviceManager()->get(interfaceName, instanceName);

    if (!retBase.isOk()) {
        std::string msg = "Cannot get " + interfaceName + "/" + instanceName + ": "
                + retBase.description();
        err << msg << std::endl;
        LOG(ERROR) << msg;
        return TRANSACTION_ERROR;
    }

    sp<IBase> base = retBase;
    if (base == nullptr) {
        std::string msg = interfaceName + "/" + instanceName + " does not exist, or "
                + "no permission to connect.";
        err << msg << std::endl;
        LOG(ERROR) << msg;
        return NO_INTERFACE;
    }

    PipeRelay relay(out);

    if (relay.initCheck() != OK) {
        std::string msg = "PipeRelay::initCheck() FAILED w/ " + std::to_string(relay.initCheck());
        err << msg << std::endl;
        LOG(ERROR) << msg;
        return IO_ERROR;
    }

    native_handle_t* fdHandle = native_handle_create(1 /* numFds */, 0 /* numInts */);
    sp<NativeHandle> spHandle = NativeHandle::create(fdHandle, true /* ownsHandle */);

    fdHandle->data[0] = relay.fd();

    hardware::Return<void> ret = base->debug(fdHandle, convert(options));

    if (!ret.isOk()) {
        std::string msg = "debug() FAILED on " + interfaceName + "/" + instanceName + ": "
                + ret.description();
        err << msg << std::endl;
        LOG(ERROR) << msg;
        return TRANSACTION_ERROR;
    }
    return OK;
}

Status Lshal::parseArgs(const Arg &arg) {
    static std::set<std::string> sAllCommands{"list", "debug", "help"};
    optind = 1;
    if (optind >= arg.argc) {
        // no options at all.
        return OK;
    }
    mCommand = arg.argv[optind];
    if (sAllCommands.find(mCommand) != sAllCommands.end()) {
        ++optind;
        return OK; // mCommand is set correctly
    }

    if (mCommand.size() > 0 && mCommand[0] == '-') {
        // first argument is an option, set command to "" (which is recognized as "list")
        mCommand = "";
        return OK;
    }

    err() << arg.argv[0] << ": unrecognized option `" << arg.argv[optind] << "'" << std::endl;
    return USAGE;
}

void signalHandler(int sig) {
    if (sig == SIGINT) {
        int retVal;
        pthread_exit(&retVal);
    }
}

std::unique_ptr<HelpCommand> Lshal::selectHelpCommand() {
    return std::make_unique<HelpCommand>(*this);
}

std::unique_ptr<Command> Lshal::selectCommand(const std::string& command) {
    // Default command is list
    if (command == "list" || command == "") {
        return std::make_unique<ListCommand>(*this);
    }
    if (command == "debug") {
        return std::make_unique<DebugCommand>(*this);
    }
    if (command == "help") {
        return selectHelpCommand();
    }
    return nullptr;
}

Status Lshal::main(const Arg &arg) {
    // Allow SIGINT to terminate all threads.
    signal(SIGINT, signalHandler);

    Status status = parseArgs(arg);
    if (status != OK) {
        usage();
        return status;
    }
    auto c = selectCommand(mCommand);
    if (c == nullptr) {
        // unknown command, print global usage
        usage();
        return USAGE;
    }
    status = c->main(arg);
    if (status == USAGE) {
        // bad options. Run `lshal help ${mCommand}` instead.
        // For example, `lshal --unknown-option` becomes `lshal help` (prints global help)
        // and `lshal list --unknown-option` becomes `lshal help list`
        return selectHelpCommand()->usageOfCommand(mCommand);
    }

    return status;
}

NullableOStream<std::ostream> Lshal::err() const {
    return mErr;
}
NullableOStream<std::ostream> Lshal::out() const {
    return mOut;
}

const sp<IServiceManager> &Lshal::serviceManager() const {
    return mServiceManager;
}

const sp<IServiceManager> &Lshal::passthroughManager() const {
    return mPassthroughManager;
}

}  // namespace lshal
}  // namespace android
