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
#include "FQName.h"
#include "HandleType.h"
#include "Interface.h"
#include "PredefinedType.h"
#include "Scope.h"
#include "TypeDef.h"

#include <hidl-util/Formatter.h>
#include <android-base/logging.h>
#include <iostream>
#include <stdlib.h>

namespace android {

AST::AST(Coordinator *coordinator, const std::string &path)
    : mCoordinator(coordinator),
      mPath(path),
      mScanner(NULL),
      mRootScope(new Scope("" /* localName */)) {
    enterScope(mRootScope);
}

AST::~AST() {
    delete mRootScope;
    mRootScope = NULL;

    CHECK(mScanner == NULL);

    // Ownership of "coordinator" was NOT transferred.
}

void *AST::scanner() {
    return mScanner;
}

void AST::setScanner(void *scanner) {
    mScanner = scanner;
}

const std::string &AST::getFilename() const {
    return mPath;
}

bool AST::setPackage(const char *package) {
    mPackage.setTo(package);
    CHECK(mPackage.isValid());

    if (mPackage.package().empty()
            || mPackage.version().empty()
            || !mPackage.name().empty()) {
        return false;
    }

    return true;
}

FQName AST::package() const {
    return mPackage;
}

bool AST::isInterface(std::string *ifaceName) const {
    return mRootScope->containsSingleInterface(ifaceName);
}

bool AST::addImport(const char *import) {
    FQName fqName(import);
    CHECK(fqName.isValid());

    fqName.applyDefaults(mPackage.package(), mPackage.version());

    // LOG(INFO) << "importing " << fqName.string();

    if (fqName.name().empty()) {
        std::vector<FQName> packageInterfaces;

        status_t err =
            mCoordinator->appendPackageInterfacesToSet(fqName,
                                                       &packageInterfaces);

        if (err != OK) {
            return false;
        }

        for (const auto &subFQName : packageInterfaces) {
            AST *ast = mCoordinator->parse(subFQName, &mImportedASTs);
            if (ast == NULL) {
                return false;
            }
        }

        return true;
    }

    AST *importAST = mCoordinator->parse(fqName, &mImportedASTs);

    if (importAST == NULL) {
        return false;
    }

    return true;
}

void AST::addImportedAST(AST *ast) {
    mImportedASTs.insert(ast);
}

void AST::enterScope(Scope *container) {
    mScopePath.push_back(container);
}

void AST::leaveScope() {
    mScopePath.pop_back();
}

Scope *AST::scope() {
    CHECK(!mScopePath.empty());
    return mScopePath.back();
}

bool AST::addTypeDef(
        const char *localName, Type *type, std::string *errorMsg) {
    // The reason we wrap the given type in a TypeDef is simply to suppress
    // emitting any type definitions later on, since this is just an alias
    // to a type defined elsewhere.
    return addScopedTypeInternal(
            new TypeDef(localName, type), errorMsg);
}

bool AST::addScopedType(NamedType *type, std::string *errorMsg) {
    return addScopedTypeInternal(
            type, errorMsg);
}

bool AST::addScopedTypeInternal(
        NamedType *type,
        std::string *errorMsg) {

    bool success = scope()->addType(type, errorMsg);
    if (!success) {
        return false;
    }

    std::string path;
    for (size_t i = 1; i < mScopePath.size(); ++i) {
        path.append(mScopePath[i]->localName());
        path.append(".");
    }
    path.append(type->localName());

    FQName fqName(mPackage.package(), mPackage.version(), path);

    type->setFullName(fqName);

    mDefinedTypesByFullName[fqName] = type;

    return true;
}

Type *AST::lookupType(const FQName &fqName) {
    CHECK(fqName.isValid());

    if (fqName.name().empty()) {
        // Given a package and version???
        return NULL;
    }

    if (fqName.package().empty() && fqName.version().empty()) {
        // This is just a plain identifier, resolve locally first if possible.

        for (size_t i = mScopePath.size(); i-- > 0;) {
            Type *type = mScopePath[i]->lookupType(fqName);

            if (type != NULL) {
                // Resolve typeDefs to the target type.
                while (type->isTypeDef()) {
                    type = static_cast<TypeDef *>(type)->referencedType();
                }

                return type->ref();
            }
        }
    }

    Type *resolvedType = nullptr;
    Type *returnedType = nullptr;
    FQName resolvedName;

    for (const auto &importedAST : mImportedASTs) {
        FQName matchingName;
        Type *match = importedAST->findDefinedType(fqName, &matchingName);

        if (match != nullptr) {
            if (resolvedType != nullptr) {
                std::cerr << "ERROR: Unable to resolve type name '"
                          << fqName.string()
                          << "', multiple matches found:\n";

                std::cerr << "  " << resolvedName.string() << "\n";
                std::cerr << "  " << matchingName.string() << "\n";

                return NULL;
            }

            resolvedType = match;
            returnedType = resolvedType;
            resolvedName = matchingName;

            // Keep going even after finding a match.
        }
    }

    if (resolvedType == nullptr
            && fqName.package().empty()
            && fqName.version().empty()
            && fqName.name() == "MQDescriptor") {
        return new PredefinedType("::android::hardware::MQDescriptor");
    }

    if (resolvedType) {
#if 0
        LOG(INFO) << "found '"
                  << resolvedName.string()
                  << "' after looking for '"
                  << fqName.string()
                  << "'.";
#endif

        // Resolve typeDefs to the target type.
        while (resolvedType->isTypeDef()) {
            resolvedType =
                static_cast<TypeDef *>(resolvedType)->referencedType();
        }

        returnedType = resolvedType;

        // If the resolved type is not an interface, we need to determine
        // whether it is defined in types.hal, or in some other interface.  In
        // the latter case, we need to emit a dependency for the interface in
        // which the type is defined.
        //
        // Consider the following:
        //    android.hardware.tests.foo@1.0::Record
        //    android.hardware.tests.foo@1.0::IFoo.Folder
        //    android.hardware.tests.foo@1.0::Folder
        //
        // If Record is an interface, then we keep track of it for the purpose
        // of emitting dependencies in the target language (for example #include
        // in C++).  If Record is a UDT, then we assume it is defined in
        // types.hal in android.hardware.tests.foo@1.0.
        //
        // In the case of IFoo.Folder, the same applies.  If IFoo is an
        // interface, we need to track this for the purpose of emitting
        // dependencies.  If not, then it must have been defined in types.hal.
        //
        // In the case of just specifying Folder, the resolved type is
        // android.hardware.tests.foo@1.0::IFoo.Folder, and the same logic as
        // above applies.

        if (!resolvedType->isInterface()) {
            FQName ifc(resolvedName.package(),
                       resolvedName.version(),
                       resolvedName.names().at(0));
            for (const auto &importedAST : mImportedASTs) {
                FQName matchingName;
                Type *match = importedAST->findDefinedType(ifc, &matchingName);
                if (match != nullptr && match->isInterface()) {
                    resolvedType = match;
                }
            }
        }

        if (!resolvedType->isInterface()) {
            // Non-interface types are declared in the associated types header.
            FQName typesName(
                    resolvedName.package(), resolvedName.version(), "types");

            mImportedNames.insert(typesName);

            if (resolvedType->isNamedType() && !resolvedType->isTypeDef()) {
                mImportedNamesForJava.insert(
                        static_cast<NamedType *>(resolvedType)->fqName());
            }
        } else {
            // Do _not_ use fqName, i.e. the name we used to look up the type,
            // but instead use the name of the interface we found.
            // This is necessary because if fqName pointed to a typedef which
            // in turn referenced the found interface we'd mistakenly use the
            // name of the typedef instead of the proper name of the interface.

            mImportedNames.insert(
                    static_cast<Interface *>(resolvedType)->fqName());

            mImportedNamesForJava.insert(
                    static_cast<Interface *>(resolvedType)->fqName());
        }
    }

    return returnedType->ref();
}

Type *AST::findDefinedType(const FQName &fqName, FQName *matchingName) const {
    for (const auto &pair : mDefinedTypesByFullName) {
        const FQName &key = pair.first;
        Type* type = pair.second;

        if (key.endsWith(fqName)) {
            *matchingName = key;
            return type;
        }
    }

    return nullptr;
}

void AST::getImportedPackages(std::set<FQName> *importSet) const {
    for (const auto &fqName : mImportedNames) {
        FQName packageName(fqName.package(), fqName.version(), "");

        if (packageName == mPackage) {
            // We only care about external imports, not our own package.
            continue;
        }

        importSet->insert(packageName);
    }
}

bool AST::isJavaCompatible() const {
    std::string ifaceName;
    if (!AST::isInterface(&ifaceName)) {
        for (const auto *type : mRootScope->getSubTypes()) {
            if (!type->isJavaCompatible()) {
                return false;
            }
        }

        return true;
    }

    const Interface *iface = mRootScope->getInterface();
    return iface->isJavaCompatible();
}

}  // namespace android;
