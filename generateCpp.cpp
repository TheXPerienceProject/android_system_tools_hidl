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

#include "AST.h"

#include "Coordinator.h"
#include "EnumType.h"
#include "Interface.h"
#include "Method.h"
#include "ScalarType.h"
#include "Scope.h"

#include <algorithm>
#include <hidl-util/Formatter.h>
#include <hidl-util/StringHelper.h>
#include <android-base/logging.h>
#include <string>
#include <vector>

namespace android {

status_t AST::generateCpp(const std::string &outputPath) const {
    status_t err = generateInterfaceHeader(outputPath);

    if (err == OK) {
        err = generateStubHeader(outputPath);
    }

    if (err == OK) {
        err = generateHwBinderHeader(outputPath);
    }

    if (err == OK) {
        err = generateProxyHeader(outputPath);
    }

    if (err == OK) {
        err = generateAllSource(outputPath);
    }

    if (err == OK) {
        err = generatePassthroughHeader(outputPath);
    }

    return err;
}

void AST::getPackageComponents(
        std::vector<std::string> *components) const {
    mPackage.getPackageComponents(components);
}

void AST::getPackageAndVersionComponents(
        std::vector<std::string> *components, bool cpp_compatible) const {
    mPackage.getPackageAndVersionComponents(components, cpp_compatible);
}

std::string AST::makeHeaderGuard(const std::string &baseName,
                                 bool indicateGenerated) const {
    std::string guard;

    if (indicateGenerated) {
        guard += "HIDL_GENERATED_";
    }

    guard += StringHelper::Uppercase(mPackage.tokenName());
    guard += "_";
    guard += StringHelper::Uppercase(baseName);
    guard += "_H";

    return guard;
}

// static
void AST::generateCppPackageInclude(
        Formatter &out,
        const FQName &package,
        const std::string &klass) {

    out << "#include <";

    std::vector<std::string> components;
    package.getPackageAndVersionComponents(&components, false /* cpp_compatible */);

    for (const auto &component : components) {
        out << component << "/";
    }

    out << klass
        << ".h>\n";
}

void AST::enterLeaveNamespace(Formatter &out, bool enter) const {
    std::vector<std::string> packageComponents;
    getPackageAndVersionComponents(
            &packageComponents, true /* cpp_compatible */);

    if (enter) {
        for (const auto &component : packageComponents) {
            out << "namespace " << component << " {\n";
        }

        out.setNamespace(mPackage.cppNamespace() + "::");
    } else {
        out.setNamespace(std::string());

        for (auto it = packageComponents.rbegin();
                it != packageComponents.rend();
                ++it) {
            out << "}  // namespace " << *it << "\n";
        }
    }
}

static void declareServiceManagerInteractions(Formatter &out, const std::string &interfaceName) {
    out << "static ::android::sp<" << interfaceName << "> getService("
        << "const std::string &serviceName=\"default\", bool getStub=false);\n";
    out << "static ::android::sp<" << interfaceName << "> getService("
        << "const char serviceName[], bool getStub=false)"
        << "  { std::string str(serviceName ? serviceName : \"\");"
        << "      return getService(str, getStub); }\n";
    out << "static ::android::sp<" << interfaceName << "> getService("
        << "const ::android::hardware::hidl_string& serviceName, bool getStub=false)"
        // without c_str the std::string constructor is ambiguous
        << "  { std::string str(serviceName.c_str());"
        << "      return getService(str, getStub); }\n";
    out << "static ::android::sp<" << interfaceName << "> getService("
        << "bool getStub) { return getService(\"default\", getStub); }\n";
    out << "::android::status_t registerAsService(const std::string &serviceName=\"default\");\n";
    out << "static bool registerForNotifications(\n";
    out.indent(2, [&] {
        out << "const std::string &serviceName,\n"
            << "const ::android::sp<::android::hidl::manager::V1_0::IServiceNotification> "
            << "&notification);\n";
    });

}

static void implementServiceManagerInteractions(Formatter &out,
        const FQName &fqName, const std::string &package) {

    const std::string interfaceName = fqName.getInterfaceName();

    out << "// static\n"
        << "::android::sp<" << interfaceName << "> " << interfaceName << "::getService("
        << "const std::string &serviceName, bool getStub) ";
    out.block([&] {
        out << "::android::sp<" << interfaceName << "> iface = nullptr;\n";
        out << "::android::vintf::Transport transport = ::android::hardware::getTransportFromManifest(\""
            << fqName.package() << "\");\n";

        out.sIf("!getStub && "
                "(transport == ::android::vintf::Transport::HWBINDER || "
                "transport == ::android::vintf::Transport::TOGGLED || "
                // TODO(b/34625838): Don't load in passthrough mode
                "transport == ::android::vintf::Transport::PASSTHROUGH || "
                "transport == ::android::vintf::Transport::EMPTY)", [&] {
            out << "const ::android::sp<::android::hidl::manager::V1_0::IServiceManager> sm\n";
            out.indent(2, [&] {
                out << "= ::android::hardware::defaultServiceManager();\n";
            });
            out.sIf("sm != nullptr", [&] {
                // TODO(b/34274385) remove sysprop check
                out.sIf("transport == ::android::vintf::Transport::HWBINDER ||"
                         "(transport == ::android::vintf::Transport::TOGGLED &&"
                         " ::android::hardware::details::blockingHalBinderizationEnabled())", [&]() {
                    out << "::android::hardware::details::waitForHwService("
                        << interfaceName << "::descriptor" << ", serviceName);\n";
                }).endl();
                out << "::android::hardware::Return<::android::sp<" << gIBaseFqName.cppName() << ">> ret = \n";
                out.indent(2, [&] {
                    out << "sm->get(" << interfaceName << "::descriptor" << ", serviceName);\n";
                });
                out.sIf("ret.isOk()", [&] {
                    out << "iface = " << interfaceName << "::castFrom(ret);\n";
                    out.sIf("iface != nullptr", [&] {
                        out << "return iface;\n";
                    }).endl();
                }).endl();
            }).endl();
        }).endl();

        out.sIf("getStub || "
                "transport == ::android::vintf::Transport::PASSTHROUGH || "
                "(transport == ::android::vintf::Transport::TOGGLED &&"
                " !::android::hardware::details::blockingHalBinderizationEnabled()) ||"
                "transport == ::android::vintf::Transport::EMPTY", [&] {
            out << "const ::android::sp<::android::hidl::manager::V1_0::IServiceManager> pm\n";
            out.indent(2, [&] {
                out << "= ::android::hardware::getPassthroughServiceManager();\n";
            });

            out.sIf("pm != nullptr", [&] () {
                out << "::android::hardware::Return<::android::sp<" << gIBaseFqName.cppName() << ">> ret = \n";
                out.indent(2, [&] {
                    out << "pm->get(" << interfaceName << "::descriptor" << ", serviceName);\n";
                });
                out.sIf("ret.isOk()", [&] {
                    out << "::android::sp<" << gIBaseFqName.cppName()
                        << "> baseInterface = ret;\n";
                    out.sIf("baseInterface != nullptr", [&]() {
                        out << "iface = new " << fqName.getInterfacePassthroughName()
                            << "(" << interfaceName << "::castFrom(baseInterface));\n";
                    });
                }).endl();
            }).endl();
        }).endl();

        out << "return iface;\n";
    }).endl().endl();

    out << "::android::status_t " << interfaceName << "::registerAsService("
        << "const std::string &serviceName) ";
    out.block([&] {
        out << "const ::android::sp<::android::hidl::manager::V1_0::IServiceManager> sm\n";
        out.indent(2, [&] {
            out << "= ::android::hardware::defaultServiceManager();\n";
        });
        out.sIf("sm == nullptr", [&] {
            out << "return ::android::INVALID_OPERATION;\n";
        }).endl();
        out << "bool success = false;\n"
            << "::android::hardware::Return<void> ret =\n";
        out.indent(2, [&] {
            out << "this->interfaceChain("
                << "[&success, &sm, &serviceName, this](const auto &chain) ";
            out.block([&] {
                out << "::android::hardware::Return<bool> addRet = "
                    << "sm->add(chain, serviceName.c_str(), this);\n";
                out << "success = addRet.isOk() && addRet;\n";
            });
            out << ");\n";
            out << "success = success && ret.isOk();\n";
        });
        out << "return success ? ::android::OK : ::android::UNKNOWN_ERROR;\n";
    }).endl().endl();

    out << "bool " << interfaceName << "::registerForNotifications(\n";
    out.indent(2, [&] {
        out << "const std::string &serviceName,\n"
            << "const ::android::sp<::android::hidl::manager::V1_0::IServiceNotification> "
            << "&notification) ";
    });
    out.block([&] {
        out << "const ::android::sp<::android::hidl::manager::V1_0::IServiceManager> sm\n";
        out.indent(2, [&] {
            out << "= ::android::hardware::defaultServiceManager();\n";
        });
        out.sIf("sm == nullptr", [&] {
            out << "return false;\n";
        }).endl();
        out << "::android::hardware::Return<bool> success =\n";
        out.indent(2, [&] {
            out << "sm->registerForNotifications(\"" << package << "::" << interfaceName << "\",\n";
            out.indent(2, [&] {
                out << "serviceName, notification);\n";
            });
        });
        out << "return success.isOk() && success;\n";
    }).endl().endl();
}

status_t AST::generateInterfaceHeader(const std::string &outputPath) const {

    std::string path = outputPath;
    path.append(mCoordinator->convertPackageRootToPath(mPackage));
    path.append(mCoordinator->getPackagePath(mPackage, true /* relative */));

    std::string ifaceName;
    bool isInterface = true;
    if (!AST::isInterface(&ifaceName)) {
        ifaceName = "types";
        isInterface = false;
    }
    path.append(ifaceName);
    path.append(".h");

    CHECK(Coordinator::MakeParentHierarchy(path));
    FILE *file = fopen(path.c_str(), "w");

    if (file == NULL) {
        return -errno;
    }

    Formatter out(file);

    const std::string guard = makeHeaderGuard(ifaceName);

    out << "#ifndef " << guard << "\n";
    out << "#define " << guard << "\n\n";

    for (const auto &item : mImportedNames) {
        generateCppPackageInclude(out, item, item.name());
    }

    if (!mImportedNames.empty()) {
        out << "\n";
    }

    if (isInterface) {
        if (isIBase()) {
            out << "// skipped #include IServiceNotification.h\n\n";
        } else {
            out << "#include <android/hidl/manager/1.0/IServiceNotification.h>\n\n";
        }
    }

    out << "#include <hidl/HidlSupport.h>\n";
    out << "#include <hidl/MQDescriptor.h>\n";

    if (isInterface) {
        out << "#include <hidl/Status.h>\n";
    }

    out << "#include <utils/NativeHandle.h>\n";
    out << "#include <utils/misc.h>\n\n"; /* for report_sysprop_change() */

    enterLeaveNamespace(out, true /* enter */);
    out << "\n";

    if (isInterface) {
        out << "struct "
            << ifaceName;

        const Interface *iface = mRootScope->getInterface();
        const Interface *superType = iface->superType();

        if (superType == NULL) {
            out << " : virtual public ::android::RefBase";
        } else {
            out << " : public "
                << superType->fullName();
        }

        out << " {\n";

        out.indent();

    }

    status_t err = emitTypeDeclarations(out);

    if (err != OK) {
        return err;
    }

    if (isInterface) {
        const Interface *iface = mRootScope->getInterface();
        const Interface *superType = iface->superType();

        out << "virtual bool isRemote() const ";
        if (!isIBase()) {
            out << "override ";
        }
        out << "{ return false; }\n\n";

        for (const auto &method : iface->methods()) {
            out << "\n";

            const bool returnsValue = !method->results().empty();
            const TypedVar *elidedReturn = method->canElideCallback();

            if (elidedReturn == nullptr && returnsValue) {
                out << "using "
                    << method->name()
                    << "_cb = std::function<void("
                    << Method::GetArgSignature(method->results(),
                                               true /* specify namespaces */)
                    << ")>;\n";
            }

            method->dumpAnnotations(out);

            if (elidedReturn) {
                out << "virtual ::android::hardware::Return<";
                out << elidedReturn->type().getCppResultType() << "> ";
            } else {
                out << "virtual ::android::hardware::Return<void> ";
            }

            out << method->name()
                << "("
                << Method::GetArgSignature(method->args(),
                                           true /* specify namespaces */);

            if (returnsValue && elidedReturn == nullptr) {
                if (!method->args().empty()) {
                    out << ", ";
                }

                out << method->name() << "_cb _hidl_cb";
            }

            out << ")";
            if (method->isHidlReserved()) {
                if (!isIBase()) {
                    out << " override";
                }
                out << " {\n";
                out.indent();
                method->cppImpl(IMPL_HEADER, out);
                out.unindent();
                out << "\n}\n";
            } else {
                out << " = 0;\n";
            }
        }

        out << "// cast static functions\n";
        std::string childTypeResult = iface->getCppResultType();

        for (const Interface *superType : iface->typeChain()) {
            out << "static "
                << childTypeResult
                << " castFrom("
                << superType->getCppArgumentType()
                << " parent"
                << ");\n";
        }

        out << "\nstatic const char* descriptor;\n\n";

        if (isIBase()) {
            out << "// skipped getService, registerAsService, registerForNotifications\n\n";
        } else {
            declareServiceManagerInteractions(out, iface->localName());
        }

        out << "private: static int hidlStaticBlock;\n";
    }

    if (isInterface) {
        out.unindent();

        out << "};\n\n";
    }

    err = mRootScope->emitGlobalTypeDeclarations(out);

    if (err != OK) {
        return err;
    }

    out << "\n";
    enterLeaveNamespace(out, false /* enter */);

    out << "\n#endif  // " << guard << "\n";

    return OK;
}

status_t AST::generateHwBinderHeader(const std::string &outputPath) const {
    std::string ifaceName;
    bool isInterface = AST::isInterface(&ifaceName);
    const Interface *iface = nullptr;
    std::string klassName{};

    if(isInterface) {
        iface = mRootScope->getInterface();
        klassName = iface->getHwName();
    } else {
        klassName = "hwtypes";
    }

    std::string path = outputPath;
    path.append(mCoordinator->convertPackageRootToPath(mPackage));
    path.append(mCoordinator->getPackagePath(mPackage, true /* relative */));
    path.append(klassName + ".h");

    FILE *file = fopen(path.c_str(), "w");

    if (file == NULL) {
        return -errno;
    }

    Formatter out(file);

    const std::string guard = makeHeaderGuard(klassName);

    out << "#ifndef " << guard << "\n";
    out << "#define " << guard << "\n\n";

    if (isInterface) {
        generateCppPackageInclude(out, mPackage, ifaceName);
    } else {
        generateCppPackageInclude(out, mPackage, "types");
    }

    out << "\n";

    for (const auto &item : mImportedNames) {
        if (item.name() == "types") {
            generateCppPackageInclude(out, item, "hwtypes");
        } else {
            generateCppPackageInclude(out, item, item.getInterfaceStubName());
            generateCppPackageInclude(out, item, item.getInterfaceProxyName());
        }
    }

    out << "\n";

    out << "#include <hidl/Status.h>\n";
    out << "#include <hwbinder/IBinder.h>\n";
    out << "#include <hwbinder/Parcel.h>\n";

    out << "\n";

    enterLeaveNamespace(out, true /* enter */);

    status_t err = mRootScope->emitGlobalHwDeclarations(out);
    if (err != OK) {
        return err;
    }

    enterLeaveNamespace(out, false /* enter */);

    out << "\n#endif  // " << guard << "\n";

    return OK;
}

status_t AST::emitTypeDeclarations(Formatter &out) const {
    return mRootScope->emitTypeDeclarations(out);
}

static void wrapPassthroughArg(Formatter &out,
        const TypedVar *arg, bool addPrefixToName,
        std::function<void(void)> handleError) {
    if (!arg->type().isInterface()) {
        return;
    }
    std::string name = (addPrefixToName ? "_hidl_out_" : "") + arg->name();
    std::string wrappedName = (addPrefixToName ? "_hidl_out_wrapped_" : "_hidl_wrapped_")
            + arg->name();
    const Interface &iface = static_cast<const Interface &>(arg->type());
    out << iface.getCppStackType() << " " << wrappedName << ";\n";
    // TODO(elsk): b/33754152 Should not wrap this if object is Bs*
    out.sIf(name + " != nullptr && !" + name + "->isRemote()", [&] {
        out << wrappedName
            << " = "
            << iface.fqName().cppName()
            << "::castFrom(::android::hardware::wrapPassthrough("
            << name << "));\n";
        out.sIf(wrappedName + " == nullptr", [&] {
            // Fatal error. Happens when the BsFoo class is not found in the binary
            // or any dynamic libraries.
            handleError();
        }).endl();
    }).sElse([&] {
        out << wrappedName << " = " << name << ";\n";
    }).endl().endl();
}

status_t AST::generatePassthroughMethod(Formatter &out,
                                        const Method *method) const {
    method->generateCppSignature(out);

    out << " {\n";
    out.indent();

    if (method->isHidlReserved()
        && method->overridesCppImpl(IMPL_PASSTHROUGH)) {
        method->cppImpl(IMPL_PASSTHROUGH, out);
        out.unindent();
        out << "}\n\n";
        return OK;
    }

    const bool returnsValue = !method->results().empty();
    const TypedVar *elidedReturn = method->canElideCallback();

    if (returnsValue && elidedReturn == nullptr) {
        generateCheckNonNull(out, "_hidl_cb");
    }

    generateCppInstrumentationCall(
            out,
            InstrumentationEvent::PASSTHROUGH_ENTRY,
            method);


    for (const auto &arg : method->args()) {
        wrapPassthroughArg(out, arg, false /* addPrefixToName */, [&] {
            out << "return ::android::hardware::Status::fromExceptionCode(\n";
            out.indent(2, [&] {
                out << "::android::hardware::Status::EX_TRANSACTION_FAILED,\n"
                    << "\"Cannot wrap passthrough interface.\");\n";
            });
        });
    }

    out << "auto _hidl_error = ::android::hardware::Void();\n";
    out << "auto _hidl_return = ";

    if (method->isOneway()) {
        out << "addOnewayTask([this, &_hidl_error";
        for (const auto &arg : method->args()) {
            out << ", "
                << (arg->type().isInterface() ? "_hidl_wrapped_" : "")
                << arg->name();
        }
        out << "] {\n";
        out.indent();
        out << "this->";
    }

    out << "mImpl->"
        << method->name()
        << "(";

    bool first = true;
    for (const auto &arg : method->args()) {
        if (!first) {
            out << ", ";
        }
        first = false;
        out << (arg->type().isInterface() ? "_hidl_wrapped_" : "") << arg->name();
    }
    if (returnsValue && elidedReturn == nullptr) {
        if (!method->args().empty()) {
            out << ", ";
        }
        out << "[&](";
        first = true;
        for (const auto &arg : method->results()) {
            if (!first) {
                out << ", ";
            }

            out << "const auto &_hidl_out_"
                << arg->name();

            first = false;
        }

        out << ") {\n";
        out.indent();
        status_t status = generateCppInstrumentationCall(
                out,
                InstrumentationEvent::PASSTHROUGH_EXIT,
                method);
        if (status != OK) {
            return status;
        }

        for (const auto &arg : method->results()) {
            wrapPassthroughArg(out, arg, true /* addPrefixToName */, [&] {
                out << "_hidl_error = ::android::hardware::Status::fromExceptionCode(\n";
                out.indent(2, [&] {
                    out << "::android::hardware::Status::EX_TRANSACTION_FAILED,\n"
                        << "\"Cannot wrap passthrough interface.\");\n";
                });
                out << "return;\n";
            });
        }

        out << "_hidl_cb(";
        first = true;
        for (const auto &arg : method->results()) {
            if (!first) {
                out << ", ";
            }
            first = false;
            out << (arg->type().isInterface() ? "_hidl_out_wrapped_" : "_hidl_out_")
                << arg->name();
        }
        out << ");\n";
        out.unindent();
        out << "});\n\n";
    } else {
        out << ");\n\n";
        if (elidedReturn != nullptr) {
            out << elidedReturn->type().getCppResultType()
                << " _hidl_out_"
                << elidedReturn->name()
                << " = _hidl_return;\n";
        }
        status_t status = generateCppInstrumentationCall(
                out,
                InstrumentationEvent::PASSTHROUGH_EXIT,
                method);
        if (status != OK) {
            return status;
        }
    }

    if (method->isOneway()) {
        out.unindent();
        out << "});\n";
    }

    out << "return _hidl_return;\n";

    out.unindent();
    out << "}\n";

    return OK;
}

status_t AST::generateMethods(Formatter &out, MethodGenerator gen) const {

    const Interface *iface = mRootScope->getInterface();

    const Interface *prevIterface = nullptr;
    for (const auto &tuple : iface->allMethodsFromRoot()) {
        const Method *method = tuple.method();
        const Interface *superInterface = tuple.interface();

        if(prevIterface != superInterface) {
            if (prevIterface != nullptr) {
                out << "\n";
            }
            out << "// Methods from "
                << superInterface->fullName()
                << " follow.\n";
            prevIterface = superInterface;
        }
        status_t err = gen(method, superInterface);

        if (err != OK) {
            return err;
        }
    }

    out << "\n";

    return OK;
}

status_t AST::generateStubHeader(const std::string &outputPath) const {
    std::string ifaceName;
    if (!AST::isInterface(&ifaceName)) {
        // types.hal does not get a stub header.
        return OK;
    }

    const Interface *iface = mRootScope->getInterface();
    const std::string klassName = iface->getStubName();

    std::string path = outputPath;
    path.append(mCoordinator->convertPackageRootToPath(mPackage));
    path.append(mCoordinator->getPackagePath(mPackage, true /* relative */));
    path.append(klassName);
    path.append(".h");

    CHECK(Coordinator::MakeParentHierarchy(path));
    FILE *file = fopen(path.c_str(), "w");

    if (file == NULL) {
        return -errno;
    }

    Formatter out(file);

    const std::string guard = makeHeaderGuard(klassName);

    out << "#ifndef " << guard << "\n";
    out << "#define " << guard << "\n\n";

    generateCppPackageInclude(out, mPackage, iface->getHwName());
    out << "\n";

    enterLeaveNamespace(out, true /* enter */);
    out << "\n";

    out << "struct "
        << klassName;
    if (iface->isIBase()) {
        out << " : public ::android::hardware::BHwBinder";
        out << ", public ::android::hardware::HidlInstrumentor {\n";
    } else {
        out << " : public "
            << gIBaseFqName.getInterfaceStubFqName().cppName()
            << " {\n";
    }

    out.indent();
    out << "explicit "
        << klassName
        << "(const ::android::sp<" << ifaceName << "> &_hidl_impl);"
        << "\n";
    out << "explicit "
        << klassName
        << "(const ::android::sp<" << ifaceName << "> &_hidl_impl,"
        << " const std::string& HidlInstrumentor_package,"
        << " const std::string& HidlInstrumentor_interface);"
        << "\n\n";
    out << "::android::status_t onTransact(\n";
    out.indent();
    out.indent();
    out << "uint32_t _hidl_code,\n";
    out << "const ::android::hardware::Parcel &_hidl_data,\n";
    out << "::android::hardware::Parcel *_hidl_reply,\n";
    out << "uint32_t _hidl_flags = 0,\n";
    out << "TransactCallback _hidl_cb = nullptr) override;\n\n";
    out.unindent();
    out.unindent();

    out << "::android::sp<" << ifaceName << "> getImpl() { return _hidl_mImpl; };\n";
    out.unindent();
    out << "private:\n";
    out.indent();
    out << "::android::sp<" << ifaceName << "> _hidl_mImpl;\n";
    out.unindent();
    out << "};\n\n";

    enterLeaveNamespace(out, false /* enter */);

    out << "\n#endif  // " << guard << "\n";

    return OK;
}

status_t AST::generateProxyHeader(const std::string &outputPath) const {
    std::string ifaceName;
    if (!AST::isInterface(&ifaceName)) {
        // types.hal does not get a proxy header.
        return OK;
    }

    const Interface *iface = mRootScope->getInterface();
    const std::string proxyName = iface->getProxyName();

    std::string path = outputPath;
    path.append(mCoordinator->convertPackageRootToPath(mPackage));
    path.append(mCoordinator->getPackagePath(mPackage, true /* relative */));
    path.append(proxyName);
    path.append(".h");

    CHECK(Coordinator::MakeParentHierarchy(path));
    FILE *file = fopen(path.c_str(), "w");

    if (file == NULL) {
        return -errno;
    }

    Formatter out(file);

    const std::string guard = makeHeaderGuard(proxyName);

    out << "#ifndef " << guard << "\n";
    out << "#define " << guard << "\n\n";

    out << "#include <hidl/HidlTransportSupport.h>\n\n";

    std::vector<std::string> packageComponents;
    getPackageAndVersionComponents(
            &packageComponents, false /* cpp_compatible */);

    generateCppPackageInclude(out, mPackage, iface->getHwName());
    out << "\n";

    enterLeaveNamespace(out, true /* enter */);
    out << "\n";

    out << "struct "
        << proxyName
        << " : public ::android::hardware::BpInterface<"
        << iface->localName()
        << ">, public ::android::hardware::HidlInstrumentor {\n";

    out.indent();

    out << "explicit "
        << proxyName
        << "(const ::android::sp<::android::hardware::IBinder> &_hidl_impl);"
        << "\n\n";

    out << "virtual bool isRemote() const override { return true; }\n\n";

    status_t err = generateMethods(out, [&](const Method *method, const Interface *) {
        method->generateCppSignature(out);
        out << " override;\n";
        return OK;
    });

    if (err != OK) {
        return err;
    }

    out.unindent();
    out << "private:\n";
    out.indent();
    out << "std::mutex _hidl_mMutex;\n"
        << "std::vector<::android::sp<::android::hardware::hidl_binder_death_recipient>>"
        << " _hidl_mDeathRecipients;\n";
    out.unindent();
    out << "};\n\n";

    enterLeaveNamespace(out, false /* enter */);

    out << "\n#endif  // " << guard << "\n";

    return OK;
}

status_t AST::generateAllSource(const std::string &outputPath) const {

    std::string path = outputPath;
    path.append(mCoordinator->convertPackageRootToPath(mPackage));
    path.append(mCoordinator->getPackagePath(mPackage, true /* relative */));

    std::string ifaceName;
    std::string baseName;

    const Interface *iface = nullptr;
    bool isInterface;
    if (!AST::isInterface(&ifaceName)) {
        baseName = "types";
        isInterface = false;
    } else {
        iface = mRootScope->getInterface();
        baseName = iface->getBaseName();
        isInterface = true;
    }

    path.append(baseName);

    if (baseName != "types") {
        path.append("All");
    }

    path.append(".cpp");

    CHECK(Coordinator::MakeParentHierarchy(path));
    FILE *file = fopen(path.c_str(), "w");

    if (file == NULL) {
        return -errno;
    }

    Formatter out(file);

    out << "#define LOG_TAG \""
        << mPackage.string() << "::" << baseName
        << "\"\n\n";

    out << "#include <android/log.h>\n";
    out << "#include <cutils/trace.h>\n";
    out << "#include <hidl/HidlTransportSupport.h>\n\n";
    if (isInterface) {
        // This is a no-op for IServiceManager itself.
        out << "#include <android/hidl/manager/1.0/IServiceManager.h>\n";

        // TODO(b/34274385) remove this
        out << "#include <hidl/LegacySupport.h>\n";

        generateCppPackageInclude(out, mPackage, iface->getProxyName());
        generateCppPackageInclude(out, mPackage, iface->getStubName());
        generateCppPackageInclude(out, mPackage, iface->getPassthroughName());

        for (const Interface *superType : iface->superTypeChain()) {
            generateCppPackageInclude(out,
                                      superType->fqName(),
                                      superType->fqName().getInterfaceProxyName());
        }

        out << "#include <hidl/ServiceManagement.h>\n";
    } else {
        generateCppPackageInclude(out, mPackage, "types");
        generateCppPackageInclude(out, mPackage, "hwtypes");
    }

    out << "\n";

    enterLeaveNamespace(out, true /* enter */);
    out << "\n";

    status_t err = generateTypeSource(out, ifaceName);

    if (err == OK && isInterface) {
        const Interface *iface = mRootScope->getInterface();

        // need to be put here, generateStubSource is using this.
        out << "const char* "
            << iface->localName()
            << "::descriptor(\""
            << iface->fqName().string()
            << "\");\n\n";

        out << "int "
            << iface->localName()
            << "::hidlStaticBlock = []() -> int {\n";
        out.indent([&] {
            out << "::android::hardware::gBnConstructorMap["
                << iface->localName()
                << "::descriptor]\n";
            out.indent(2, [&] {
                out << "= [](void *iIntf) -> ::android::sp<::android::hardware::IBinder> {\n";
                out.indent([&] {
                    out << "return new "
                        << iface->getStubName()
                        << "(reinterpret_cast<"
                        << iface->localName()
                        << " *>(iIntf));\n";
                });
                out << "};\n";
            });
            out << "::android::hardware::gBsConstructorMap["
                << iface->localName()
                << "::descriptor]\n";
            out.indent(2, [&] {
                out << "= [](void *iIntf) -> ::android::sp<"
                    << gIBaseFqName.cppName()
                    << "> {\n";
                out.indent([&] {
                    out << "return new "
                        << iface->getPassthroughName()
                        << "(reinterpret_cast<"
                        << iface->localName()
                        << " *>(iIntf));\n";
                });
                out << "};\n";
            });
            out << "return 1;\n";
        });
        out << "}();\n\n";

        err = generateInterfaceSource(out);
    }

    if (err == OK && isInterface) {
        err = generateProxySource(out, iface->fqName());
    }

    if (err == OK && isInterface) {
        err = generateStubSource(out, iface);
    }

    if (err == OK && isInterface) {
        err = generatePassthroughSource(out);
    }

    if (err == OK && isInterface) {
        const Interface *iface = mRootScope->getInterface();

        if (isIBase()) {
            out << "// skipped getService, registerAsService, registerForNotifications\n";
        } else {
            std::string package = iface->fqName().package()
                    + iface->fqName().atVersion();

            implementServiceManagerInteractions(out, iface->fqName(), package);
        }
    }

    enterLeaveNamespace(out, false /* enter */);

    return err;
}

// static
void AST::generateCheckNonNull(Formatter &out, const std::string &nonNull) {
    out.sIf(nonNull + " == nullptr", [&] {
        out << "return ::android::hardware::Status::fromExceptionCode(\n";
        out.indent(2, [&] {
            out << "::android::hardware::Status::EX_ILLEGAL_ARGUMENT);\n";
        });
    }).endl().endl();
}

status_t AST::generateTypeSource(
        Formatter &out, const std::string &ifaceName) const {
    return mRootScope->emitTypeDefinitions(out, ifaceName);
}

void AST::declareCppReaderLocals(
        Formatter &out,
        const std::vector<TypedVar *> &args,
        bool forResults) const {
    if (args.empty()) {
        return;
    }

    for (const auto &arg : args) {
        const Type &type = arg->type();

        out << type.getCppResultType()
            << " "
            << (forResults ? "_hidl_out_" : "") + arg->name()
            << ";\n";
    }

    out << "\n";
}

void AST::emitCppReaderWriter(
        Formatter &out,
        const std::string &parcelObj,
        bool parcelObjIsPointer,
        const TypedVar *arg,
        bool isReader,
        Type::ErrorMode mode,
        bool addPrefixToName) const {
    const Type &type = arg->type();

    type.emitReaderWriter(
            out,
            addPrefixToName ? ("_hidl_out_" + arg->name()) : arg->name(),
            parcelObj,
            parcelObjIsPointer,
            isReader,
            mode);
}

void AST::emitCppResolveReferences(
        Formatter &out,
        const std::string &parcelObj,
        bool parcelObjIsPointer,
        const TypedVar *arg,
        bool isReader,
        Type::ErrorMode mode,
        bool addPrefixToName) const {
    const Type &type = arg->type();
    if(type.needsResolveReferences()) {
        type.emitResolveReferences(
                out,
                addPrefixToName ? ("_hidl_out_" + arg->name()) : arg->name(),
                isReader, // nameIsPointer
                parcelObj,
                parcelObjIsPointer,
                isReader,
                mode);
    }
}

status_t AST::generateProxyMethodSource(Formatter &out,
                                        const std::string &klassName,
                                        const Method *method,
                                        const Interface *superInterface) const {

    method->generateCppSignature(out,
                                 klassName,
                                 true /* specify namespaces */);

    const bool returnsValue = !method->results().empty();
    const TypedVar *elidedReturn = method->canElideCallback();

    out << " {\n";

    out.indent();

    if (method->isHidlReserved() && method->overridesCppImpl(IMPL_PROXY)) {
        method->cppImpl(IMPL_PROXY, out);
        out.unindent();
        out << "}\n\n";
        return OK;
    }

    if (returnsValue && elidedReturn == nullptr) {
        generateCheckNonNull(out, "_hidl_cb");
    }

    status_t status = generateCppInstrumentationCall(
            out,
            InstrumentationEvent::CLIENT_API_ENTRY,
            method);
    if (status != OK) {
        return status;
    }

    out << "::android::hardware::Parcel _hidl_data;\n";
    out << "::android::hardware::Parcel _hidl_reply;\n";
    out << "::android::status_t _hidl_err;\n";
    out << "::android::hardware::Status _hidl_status;\n\n";

    declareCppReaderLocals(
            out, method->results(), true /* forResults */);

    out << "_hidl_err = _hidl_data.writeInterfaceToken(";
    out << superInterface->fqName().cppName();
    out << "::descriptor);\n";
    out << "if (_hidl_err != ::android::OK) { goto _hidl_error; }\n\n";

    bool hasInterfaceArgument = false;
    // First DFS: write all buffers and resolve pointers for parent
    for (const auto &arg : method->args()) {
        if (arg->type().isInterface()) {
            hasInterfaceArgument = true;
        }
        emitCppReaderWriter(
                out,
                "_hidl_data",
                false /* parcelObjIsPointer */,
                arg,
                false /* reader */,
                Type::ErrorMode_Goto,
                false /* addPrefixToName */);
    }

    // Second DFS: resolve references.
    for (const auto &arg : method->args()) {
        emitCppResolveReferences(
                out,
                "_hidl_data",
                false /* parcelObjIsPointer */,
                arg,
                false /* reader */,
                Type::ErrorMode_Goto,
                false /* addPrefixToName */);
    }

    if (hasInterfaceArgument) {
        // Start binder threadpool to handle incoming transactions
        out << "::android::hardware::ProcessState::self()->startThreadPool();\n";
    }
    out << "_hidl_err = remote()->transact("
        << method->getSerialId()
        << " /* "
        << method->name()
        << " */, _hidl_data, &_hidl_reply";

    if (method->isOneway()) {
        out << ", ::android::hardware::IBinder::FLAG_ONEWAY";
    }
    out << ");\n";

    out << "if (_hidl_err != ::android::OK) { goto _hidl_error; }\n\n";

    if (!method->isOneway()) {
        out << "_hidl_err = ::android::hardware::readFromParcel(&_hidl_status, _hidl_reply);\n";
        out << "if (_hidl_err != ::android::OK) { goto _hidl_error; }\n\n";
        out << "if (!_hidl_status.isOk()) { return _hidl_status; }\n\n";


        // First DFS: write all buffers and resolve pointers for parent
        for (const auto &arg : method->results()) {
            emitCppReaderWriter(
                    out,
                    "_hidl_reply",
                    false /* parcelObjIsPointer */,
                    arg,
                    true /* reader */,
                    Type::ErrorMode_Goto,
                    true /* addPrefixToName */);
        }

        // Second DFS: resolve references.
        for (const auto &arg : method->results()) {
            emitCppResolveReferences(
                    out,
                    "_hidl_reply",
                    false /* parcelObjIsPointer */,
                    arg,
                    true /* reader */,
                    Type::ErrorMode_Goto,
                    true /* addPrefixToName */);
        }

        if (returnsValue && elidedReturn == nullptr) {
            out << "_hidl_cb(";

            bool first = true;
            for (const auto &arg : method->results()) {
                if (!first) {
                    out << ", ";
                }

                if (arg->type().resultNeedsDeref()) {
                    out << "*";
                }
                out << "_hidl_out_" << arg->name();

                first = false;
            }

            out << ");\n\n";
        }
    }
    status = generateCppInstrumentationCall(
            out,
            InstrumentationEvent::CLIENT_API_EXIT,
            method);
    if (status != OK) {
        return status;
    }

    if (elidedReturn != nullptr) {
        out << "_hidl_status.setFromStatusT(_hidl_err);\n";
        out << "return ::android::hardware::Return<";
        out << elidedReturn->type().getCppResultType()
            << ">(_hidl_out_" << elidedReturn->name() << ");\n\n";
    } else {
        out << "_hidl_status.setFromStatusT(_hidl_err);\n";
        out << "return ::android::hardware::Return<void>();\n\n";
    }

    out.unindent();
    out << "_hidl_error:\n";
    out.indent();
    out << "_hidl_status.setFromStatusT(_hidl_err);\n";
    out << "return ::android::hardware::Return<";
    if (elidedReturn != nullptr) {
        out << method->results().at(0)->type().getCppResultType();
    } else {
        out << "void";
    }
    out << ">(_hidl_status);\n";

    out.unindent();
    out << "}\n\n";
    return OK;
}

status_t AST::generateProxySource(
        Formatter &out, const FQName &fqName) const {
    const std::string klassName = fqName.getInterfaceProxyName();

    out << klassName
        << "::"
        << klassName
        << "(const ::android::sp<::android::hardware::IBinder> &_hidl_impl)\n";

    out.indent();
    out.indent();

    out << ": BpInterface"
        << "<"
        << fqName.getInterfaceName()
        << ">(_hidl_impl),\n"
        << "  ::android::hardware::HidlInstrumentor(\""
        << mPackage.string()
        << "\", \""
        << fqName.getInterfaceName()
        << "\") {\n";

    out.unindent();
    out.unindent();
    out << "}\n\n";

    status_t err = generateMethods(out, [&](const Method *method, const Interface *superInterface) {
        return generateProxyMethodSource(out, klassName, method, superInterface);
    });

    return err;
}

status_t AST::generateStubSource(
        Formatter &out,
        const Interface *iface) const {
    const std::string interfaceName = iface->localName();
    const std::string klassName = iface->getStubName();

    out << klassName
        << "::"
        << klassName
        << "(const ::android::sp<" << interfaceName <<"> &_hidl_impl)\n";

    out.indent();
    out.indent();

    if (iface->isIBase()) {
        out << ": ::android::hardware::HidlInstrumentor(\"";
    } else {
        out << ": "
            << gIBaseFqName.getInterfaceStubFqName().cppName()
            << "(_hidl_impl, \"";
    }

    out << mPackage.string()
        << "\", \""
        << interfaceName
        << "\") { \n";
    out.indent();
    out << "_hidl_mImpl = _hidl_impl;\n";
    out.unindent();

    out.unindent();
    out.unindent();
    out << "}\n\n";

    if (iface->isIBase()) {
        // BnHwBase has a constructor to initialize the HidlInstrumentor
        // class properly.
        out << klassName
            << "::"
            << klassName
            << "(const ::android::sp<" << interfaceName << "> &_hidl_impl,"
            << " const std::string &HidlInstrumentor_package,"
            << " const std::string &HidlInstrumentor_interface)\n";

        out.indent();
        out.indent();

        out << ": ::android::hardware::HidlInstrumentor("
            << "HidlInstrumentor_package, HidlInstrumentor_interface) {\n";
        out.indent();
        out << "_hidl_mImpl = _hidl_impl;\n";
        out.unindent();

        out.unindent();
        out.unindent();
        out << "}\n\n";
    }


    out << "::android::status_t " << klassName << "::onTransact(\n";

    out.indent();
    out.indent();

    out << "uint32_t _hidl_code,\n"
        << "const ::android::hardware::Parcel &_hidl_data,\n"
        << "::android::hardware::Parcel *_hidl_reply,\n"
        << "uint32_t _hidl_flags,\n"
        << "TransactCallback _hidl_cb) {\n";

    out.unindent();

    out << "::android::status_t _hidl_err = ::android::OK;\n\n";
    out << "switch (_hidl_code) {\n";
    out.indent();

    for (const auto &tuple : iface->allMethodsFromRoot()) {
        const Method *method = tuple.method();
        const Interface *superInterface = tuple.interface();
        out << "case "
            << method->getSerialId()
            << " /* "
            << method->name()
            << " */:\n{\n";

        out.indent();

        status_t err =
            generateStubSourceForMethod(out, superInterface, method);

        if (err != OK) {
            return err;
        }

        out.unindent();
        out << "}\n\n";
    }

    out << "default:\n{\n";
    out.indent();

    out << "return onTransact(\n";

    out.indent();
    out.indent();

    out << "_hidl_code, _hidl_data, _hidl_reply, "
        << "_hidl_flags, _hidl_cb);\n";

    out.unindent();
    out.unindent();

    out.unindent();
    out << "}\n";

    out.unindent();
    out << "}\n\n";

    out.sIf("_hidl_err == ::android::UNEXPECTED_NULL", [&] {
        out << "_hidl_err = ::android::hardware::writeToParcel(\n";
        out.indent(2, [&] {
            out << "::android::hardware::Status::fromExceptionCode(::android::hardware::Status::EX_NULL_POINTER),\n";
            out << "_hidl_reply);\n";
        });
    });

    out << "return _hidl_err;\n";

    out.unindent();
    out << "}\n\n";

    return OK;
}

status_t AST::generateStubSourceForMethod(
        Formatter &out, const Interface *iface, const Method *method) const {
    if (method->isHidlReserved() && method->overridesCppImpl(IMPL_STUB)) {
        method->cppImpl(IMPL_STUB, out);
        out << "break;\n";
        return OK;
    }

    out << "if (!_hidl_data.enforceInterface("
        << iface->fullName()
        << "::descriptor)) {\n";

    out.indent();
    out << "_hidl_err = ::android::BAD_TYPE;\n";
    out << "break;\n";
    out.unindent();
    out << "}\n\n";

    declareCppReaderLocals(out, method->args(), false /* forResults */);

    // First DFS: write buffers
    for (const auto &arg : method->args()) {
        emitCppReaderWriter(
                out,
                "_hidl_data",
                false /* parcelObjIsPointer */,
                arg,
                true /* reader */,
                Type::ErrorMode_Break,
                false /* addPrefixToName */);
    }

    // Second DFS: resolve references
    for (const auto &arg : method->args()) {
        emitCppResolveReferences(
                out,
                "_hidl_data",
                false /* parcelObjIsPointer */,
                arg,
                true /* reader */,
                Type::ErrorMode_Break,
                false /* addPrefixToName */);
    }

    status_t status = generateCppInstrumentationCall(
            out,
            InstrumentationEvent::SERVER_API_ENTRY,
            method);
    if (status != OK) {
        return status;
    }

    const bool returnsValue = !method->results().empty();
    const TypedVar *elidedReturn = method->canElideCallback();

    if (elidedReturn != nullptr) {
        out << elidedReturn->type().getCppResultType()
            << " _hidl_out_"
            << elidedReturn->name()
            << " = "
            << "_hidl_mImpl->" << method->name()
            << "(";

        bool first = true;
        for (const auto &arg : method->args()) {
            if (!first) {
                out << ", ";
            }

            if (arg->type().resultNeedsDeref()) {
                out << "*";
            }

            out << arg->name();

            first = false;
        }

        out << ");\n\n";
        out << "::android::hardware::writeToParcel(::android::hardware::Status::ok(), "
            << "_hidl_reply);\n\n";

        elidedReturn->type().emitReaderWriter(
                out,
                "_hidl_out_" + elidedReturn->name(),
                "_hidl_reply",
                true, /* parcelObjIsPointer */
                false, /* isReader */
                Type::ErrorMode_Ignore);

        emitCppResolveReferences(
                out,
                "_hidl_reply",
                true /* parcelObjIsPointer */,
                elidedReturn,
                false /* reader */,
                Type::ErrorMode_Ignore,
                true /* addPrefixToName */);

        status_t status = generateCppInstrumentationCall(
                out,
                InstrumentationEvent::SERVER_API_EXIT,
                method);
        if (status != OK) {
            return status;
        }

        out << "_hidl_cb(*_hidl_reply);\n";
    } else {
        if (returnsValue) {
            out << "bool _hidl_callbackCalled = false;\n\n";
        }

        out << "_hidl_mImpl->" << method->name() << "(";

        bool first = true;
        for (const auto &arg : method->args()) {
            if (!first) {
                out << ", ";
            }

            if (arg->type().resultNeedsDeref()) {
                out << "*";
            }

            out << arg->name();

            first = false;
        }

        if (returnsValue) {
            if (!first) {
                out << ", ";
            }

            out << "[&](";

            first = true;
            for (const auto &arg : method->results()) {
                if (!first) {
                    out << ", ";
                }

                out << "const auto &_hidl_out_" << arg->name();

                first = false;
            }

            out << ") {\n";
            out.indent();
            out << "if (_hidl_callbackCalled) {\n";
            out.indent();
            out << "LOG_ALWAYS_FATAL(\""
                << method->name()
                << ": _hidl_cb called a second time, but must be called once.\");\n";
            out.unindent();
            out << "}\n";
            out << "_hidl_callbackCalled = true;\n\n";

            out << "::android::hardware::writeToParcel(::android::hardware::Status::ok(), "
                << "_hidl_reply);\n\n";

            // First DFS: buffers
            for (const auto &arg : method->results()) {
                emitCppReaderWriter(
                        out,
                        "_hidl_reply",
                        true /* parcelObjIsPointer */,
                        arg,
                        false /* reader */,
                        Type::ErrorMode_Ignore,
                        true /* addPrefixToName */);
            }

            // Second DFS: resolve references
            for (const auto &arg : method->results()) {
                emitCppResolveReferences(
                        out,
                        "_hidl_reply",
                        true /* parcelObjIsPointer */,
                        arg,
                        false /* reader */,
                        Type::ErrorMode_Ignore,
                        true /* addPrefixToName */);
            }

            status_t status = generateCppInstrumentationCall(
                    out,
                    InstrumentationEvent::SERVER_API_EXIT,
                    method);
            if (status != OK) {
                return status;
            }

            out << "_hidl_cb(*_hidl_reply);\n";

            out.unindent();
            out << "});\n\n";
        } else {
            out << ");\n\n";
            status_t status = generateCppInstrumentationCall(
                    out,
                    InstrumentationEvent::SERVER_API_EXIT,
                    method);
            if (status != OK) {
                return status;
            }
        }

        if (returnsValue) {
            out << "if (!_hidl_callbackCalled) {\n";
            out.indent();
            out << "LOG_ALWAYS_FATAL(\""
                << method->name()
                << ": _hidl_cb not called, but must be called once.\");\n";
            out.unindent();
            out << "}\n\n";
        } else {
            out << "::android::hardware::writeToParcel("
                << "::android::hardware::Status::ok(), "
                << "_hidl_reply);\n\n";
        }
    }

    out << "break;\n";

    return OK;
}

status_t AST::generatePassthroughHeader(const std::string &outputPath) const {
    std::string ifaceName;
    if (!AST::isInterface(&ifaceName)) {
        // types.hal does not get a stub header.
        return OK;
    }

    const Interface *iface = mRootScope->getInterface();

    const std::string klassName = iface->getPassthroughName();

    bool supportOneway = iface->hasOnewayMethods();

    std::string path = outputPath;
    path.append(mCoordinator->convertPackageRootToPath(mPackage));
    path.append(mCoordinator->getPackagePath(mPackage, true /* relative */));
    path.append(klassName);
    path.append(".h");

    CHECK(Coordinator::MakeParentHierarchy(path));
    FILE *file = fopen(path.c_str(), "w");

    if (file == NULL) {
        return -errno;
    }

    Formatter out(file);

    const std::string guard = makeHeaderGuard(klassName);

    out << "#ifndef " << guard << "\n";
    out << "#define " << guard << "\n\n";

    std::vector<std::string> packageComponents;
    getPackageAndVersionComponents(
            &packageComponents, false /* cpp_compatible */);

    out << "#include <cutils/trace.h>\n";
    out << "#include <future>\n";

    generateCppPackageInclude(out, mPackage, ifaceName);
    out << "\n";

    out << "#include <hidl/HidlPassthroughSupport.h>\n";
    if (supportOneway) {
        out << "#include <hidl/TaskRunner.h>\n";
    }

    enterLeaveNamespace(out, true /* enter */);
    out << "\n";

    out << "struct "
        << klassName
        << " : " << ifaceName
        << ", ::android::hardware::HidlInstrumentor {\n";

    out.indent();
    out << "explicit "
        << klassName
        << "(const ::android::sp<"
        << ifaceName
        << "> impl);\n";

    status_t err = generateMethods(out, [&](const Method *method, const Interface *) {
        return generatePassthroughMethod(out, method);
    });

    if (err != OK) {
        return err;
    }

    out.unindent();
    out << "private:\n";
    out.indent();
    out << "const ::android::sp<" << ifaceName << "> mImpl;\n";

    if (supportOneway) {
        out << "::android::hardware::TaskRunner mOnewayQueue;\n";

        out << "\n";

        out << "::android::hardware::Return<void> addOnewayTask("
               "std::function<void(void)>);\n\n";
    }

    out.unindent();

    out << "};\n\n";

    enterLeaveNamespace(out, false /* enter */);

    out << "\n#endif  // " << guard << "\n";

    return OK;
}

status_t AST::generateInterfaceSource(Formatter &out) const {
    const Interface *iface = mRootScope->getInterface();

    // generate castFrom functions
    std::string childTypeResult = iface->getCppResultType();

    for (const Interface *superType : iface->typeChain()) {
        out << "// static \n"
            << childTypeResult
            << " "
            << iface->localName()
            << "::castFrom("
            << superType->getCppArgumentType()
            << " parent) {\n";
        out.indent();
        if (iface == superType) {
            out << "return parent;\n";
        } else {
            out << "return ::android::hardware::castInterface<";
            out << iface->localName() << ", "
                << superType->fqName().cppName() << ", "
                << iface->getProxyName() << ", "
                << superType->getProxyFqName().cppName()
                << ">(\n";
            out.indent();
            out.indent();
            out << "parent, \""
                << iface->fqName().string()
                << "\");\n";
            out.unindent();
            out.unindent();
        }
        out.unindent();
        out << "}\n\n";
    }

    return OK;
}

status_t AST::generatePassthroughSource(Formatter &out) const {
    const Interface *iface = mRootScope->getInterface();

    const std::string klassName = iface->getPassthroughName();

    out << klassName
        << "::"
        << klassName
        << "(const ::android::sp<"
        << iface->fullName()
        << "> impl) : ::android::hardware::HidlInstrumentor(\""
        << mPackage.string()
        << "\", \""
        << iface->localName()
        << "\"), mImpl(impl) {";
    if (iface->hasOnewayMethods()) {
        out << "\n";
        out.indent([&] {
            out << "mOnewayQueue.setLimit(3000 /* similar limit to binderized */);\n";
        });
    }
    out << "}\n\n";

    if (iface->hasOnewayMethods()) {
        out << "::android::hardware::Return<void> "
            << klassName
            << "::addOnewayTask(std::function<void(void)> fun) {\n";
        out.indent();
        out << "if (!mOnewayQueue.push(fun)) {\n";
        out.indent();
        out << "return ::android::hardware::Status::fromExceptionCode(\n";
        out.indent();
        out.indent();
        out << "::android::hardware::Status::EX_TRANSACTION_FAILED);\n";
        out.unindent();
        out.unindent();
        out.unindent();
        out << "}\n";

        out << "return ::android::hardware::Status();\n";

        out.unindent();
        out << "}\n\n";


    }

    return OK;
}

status_t AST::generateCppAtraceCall(Formatter &out,
                                    InstrumentationEvent event,
                                    const Method *method) const {
    const Interface *iface = mRootScope->getInterface();
    std::string baseString = "HIDL::" + iface->localName() + "::" + method->name();
    switch (event) {
        case SERVER_API_ENTRY:
        {
            out << "atrace_begin(ATRACE_TAG_HAL, \""
                << baseString + "::server\");\n";
            break;
        }
        case CLIENT_API_ENTRY:
        {
            out << "atrace_begin(ATRACE_TAG_HAL, \""
                << baseString + "::client\");\n";
            break;
        }
        case PASSTHROUGH_ENTRY:
        {
            out << "atrace_begin(ATRACE_TAG_HAL, \""
                << baseString + "::passthrough\");\n";
            break;
        }
        case SERVER_API_EXIT:
        case CLIENT_API_EXIT:
        case PASSTHROUGH_EXIT:
        {
            out << "atrace_end(ATRACE_TAG_HAL);\n";
            break;
        }
        default:
        {
            LOG(ERROR) << "Unsupported instrumentation event: " << event;
            return UNKNOWN_ERROR;
        }
    }

    return OK;
}

status_t AST::generateCppInstrumentationCall(
        Formatter &out,
        InstrumentationEvent event,
        const Method *method) const {
    status_t err = generateCppAtraceCall(out, event, method);
    if (err != OK) {
        return err;
    }

    out << "if (UNLIKELY(mEnableInstrumentation)) {\n";
    out.indent();
    out << "std::vector<void *> _hidl_args;\n";
    std::string event_str = "";
    switch (event) {
        case SERVER_API_ENTRY:
        {
            event_str = "InstrumentationEvent::SERVER_API_ENTRY";
            for (const auto &arg : method->args()) {
                out << "_hidl_args.push_back((void *)"
                    << (arg->type().resultNeedsDeref() ? "" : "&")
                    << arg->name()
                    << ");\n";
            }
            break;
        }
        case SERVER_API_EXIT:
        {
            event_str = "InstrumentationEvent::SERVER_API_EXIT";
            for (const auto &arg : method->results()) {
                out << "_hidl_args.push_back((void *)&_hidl_out_"
                    << arg->name()
                    << ");\n";
            }
            break;
        }
        case CLIENT_API_ENTRY:
        {
            event_str = "InstrumentationEvent::CLIENT_API_ENTRY";
            for (const auto &arg : method->args()) {
                out << "_hidl_args.push_back((void *)&"
                    << arg->name()
                    << ");\n";
            }
            break;
        }
        case CLIENT_API_EXIT:
        {
            event_str = "InstrumentationEvent::CLIENT_API_EXIT";
            for (const auto &arg : method->results()) {
                out << "_hidl_args.push_back((void *)"
                    << (arg->type().resultNeedsDeref() ? "" : "&")
                    << "_hidl_out_"
                    << arg->name()
                    << ");\n";
            }
            break;
        }
        case PASSTHROUGH_ENTRY:
        {
            event_str = "InstrumentationEvent::PASSTHROUGH_ENTRY";
            for (const auto &arg : method->args()) {
                out << "_hidl_args.push_back((void *)&"
                    << arg->name()
                    << ");\n";
            }
            break;
        }
        case PASSTHROUGH_EXIT:
        {
            event_str = "InstrumentationEvent::PASSTHROUGH_EXIT";
            for (const auto &arg : method->results()) {
                out << "_hidl_args.push_back((void *)&_hidl_out_"
                    << arg->name()
                    << ");\n";
            }
            break;
        }
        default:
        {
            LOG(ERROR) << "Unsupported instrumentation event: " << event;
            return UNKNOWN_ERROR;
        }
    }

    const Interface *iface = mRootScope->getInterface();

    out << "for (const auto &callback: mInstrumentationCallbacks) {\n";
    out.indent();
    out << "callback("
        << event_str
        << ", \""
        << mPackage.package()
        << "\", \""
        << mPackage.version()
        << "\", \""
        << iface->localName()
        << "\", \""
        << method->name()
        << "\", &_hidl_args);\n";
    out.unindent();
    out << "}\n";
    out.unindent();
    out << "}\n\n";

    return OK;
}

}  // namespace android
