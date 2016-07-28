#ifndef SCALAR_TYPE_H_

#define SCALAR_TYPE_H_

#include "Type.h"

namespace android {

struct ScalarType : public Type {
    enum Kind {
        KIND_CHAR,
        KIND_BOOL,
        KIND_OPAQUE,
        KIND_INT8,
        KIND_UINT8,
        KIND_INT16,
        KIND_UINT16,
        KIND_INT32,
        KIND_UINT32,
        KIND_INT64,
        KIND_UINT64,
        KIND_FLOAT,
        KIND_DOUBLE,
    };

    ScalarType(Kind kind);

    void dump(Formatter &out) const override;

private:
    Kind mKind;

    DISALLOW_COPY_AND_ASSIGN(ScalarType);
};

}  // namespace android

#endif  // SCALAR_TYPE_H_
