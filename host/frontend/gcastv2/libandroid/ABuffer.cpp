/*
 * Copyright (C) 2010 The Android Open Source Project
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

#include <media/stagefright/foundation/ABuffer.h>

#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>

namespace android {

ABuffer::ABuffer(size_t capacity)
    : mData(malloc(capacity)),
      mCapacity(capacity),
      mRangeOffset(0),
      mRangeLength(capacity) {
}

ABuffer::~ABuffer() {
    if (mData != NULL) {
        free(mData);
        mData = NULL;
    }
}

void ABuffer::setRange(size_t offset, size_t size) {
    CHECK_LE(offset, mCapacity);
    CHECK_LE(offset + size, mCapacity);

    mRangeOffset = offset;
    mRangeLength = size;
}

std::shared_ptr<AMessage> ABuffer::meta() {
    if (!mMeta) {
        mMeta.reset(new AMessage);
    }
    return mMeta;
}

void ABuffer::reserve(size_t size) {
    if (mCapacity >= size) {
        return;
    }

    mCapacity = size;
    void *newData = realloc(mData, mCapacity);
    if (!newData) {
        newData = malloc(mCapacity);

        memcpy(static_cast<uint8_t *>(newData) + mRangeOffset,
               this->data(),
               this->size());

        free(mData);
    }
    mData = newData;
}

}  // namespace android

