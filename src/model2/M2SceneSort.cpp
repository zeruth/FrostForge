#include <cstring>

#include "model2/M2Element.hpp"
#include "model2/M2Model.hpp"
#include "model2/M2Particle.hpp"
#include "model2/M2Scene.hpp"
#include "model2/M2Shared.hpp"

// Engine/Source/Model2/M2Scene.cpp: the draw order. The frame's lists are merged into the
// scene's element array and per-pass index lists, instanced elements are grouped, and each
// pass is sorted by its own order.

typedef int32_t (*M2ELEMENTCOMPARE)(uint32_t a, uint32_t b, const M2Element* elements);

void M2IndicesReserve(M2SceneVector<uint32_t>* indices, uint64_t count);
void M2IndicesPush(M2SceneVector<uint32_t>* indices, uint32_t value);
void M2ElementsReserve(M2SceneVector<M2Element>* elements, uint64_t count);
void M2SceneEntriesReserveElements(M2SceneVector<M2Element>* elements, uint64_t count);

namespace {

// The median of three by `compare` (the pattern FUN_141204d30 uses three times and once more).
uint32_t* Median3(uint32_t* a, uint32_t* b, uint32_t* c, M2ELEMENTCOMPARE compare, const M2Element* context) {
    if (compare(*a, *b, context) < 0) {
        if (compare(*b, *c, context) < 0) {
            return b;
        }
        return compare(*a, *c, context) < 0 ? c : a;
    }
    if (compare(*a, *c, context) < 0) {
        return a;
    }
    return compare(*b, *c, context) < 0 ? c : b;
}

// The median of the medians of three spread triples (FUN_141204d30).
uint32_t* GuessMedian(uint32_t* first, uint32_t* last, M2ELEMENTCOMPARE compare, const M2Element* context) {
    uint32_t* lastElement = last - 1;
    int64_t count = last - first;
    uint32_t* mid = first + count / 2;
    int64_t span = lastElement - first + 1;
    int64_t step = span / 8;
    uint32_t* low = Median3(first, first + step, first + step * 2, compare, context);
    uint32_t* middle = Median3(mid - step, mid, mid + step, compare, context);
    uint32_t* high = Median3(lastElement - step * 2, lastElement - step, lastElement, compare, context);
    return Median3(low, middle, high, compare, context);
}

// The heap sort the introsort falls back to (FUN_141204f30).
void HeapSort(uint32_t* first, uint32_t* last, M2ELEMENTCOMPARE compare, const M2Element* context) {
    int64_t count = last - first;
    if (2 <= count) {
        int64_t top = count / 2 - 1;
        int64_t child0 = top * 2 + 2;
        while (true) {
            uint32_t value = first[top];
            int64_t hole = top;
            int64_t child = child0;
            while (child < count) {
                int64_t pick = child - 1;
                if (0 <= compare(first[child], first[child - 1], context)) {
                    pick = child;
                }
                first[hole] = first[pick];
                hole = pick;
                child = pick * 2 + 2;
            }
            if (child == count) {
                first[hole] = first[child - 1];
                hole = child - 1;
            }
            while (top < hole) {
                int64_t parent = (hole - 1) >> 1;
                if (0 <= compare(first[parent], value, context)) {
                    break;
                }
                first[hole] = first[parent];
                hole = parent;
            }
            first[hole] = value;
            if (top == 0) {
                break;
            }
            top--;
            child0 -= 2;
        }
    }
    if (1 < count) {
        int64_t size;
        do {
            uint32_t value = last[-1];
            last--;
            size = last - first;
            *last = *first;
            int64_t hole = 0;
            int64_t child = 2;
            bool single = size == 2;
            int64_t at = 0;
            if (2 < size) {
                do {
                    hole = child - 1;
                    if (0 <= compare(first[child], first[child - 1], context)) {
                        hole = child;
                    }
                    child = hole * 2 + 2;
                    first[at] = first[hole];
                    single = child == size;
                    at = hole;
                } while (child < size);
            }
            if (single) {
                first[hole] = first[child - 1];
                hole = child - 1;
            }
            while (0 < hole) {
                int64_t parent = (hole - 1) >> 1;
                if (0 <= compare(first[parent], value, context)) {
                    break;
                }
                first[hole] = first[parent];
                hole = parent;
            }
            first[hole] = value;
        } while (1 < size);
    }
}

// The introsort: a three-way partition about a guessed median, recursing on the left and
// looping on the right, heap sort past the depth, insertion sort below 33 (FUN_141204aa0).
// Its move of the left equal block leaves the first equal element where it is.
void IntroSort(uint32_t* first, uint32_t* last, int64_t ideal, M2ELEMENTCOMPARE compare, const M2Element* context) {
    while (32 < last - first && 0 < ideal) {
        uint32_t* pivot = last - 1;
        uint32_t* median = GuessMedian(first, last, compare, context);
        uint32_t swap = *median;
        *median = *pivot;
        *pivot = swap;
        uint32_t* up = first;
        uint32_t* down = pivot;
        uint32_t* rightEqual = pivot;
        uint32_t* leftEqual = first;
        while (true) {
            while (compare(*up, *pivot, context) < 0) {
                up++;
            }
            do {
                down--;
                if (0 <= compare(*pivot, *down, context)) {
                    break;
                }
            } while (down != first);
            if (down <= up) {
                break;
            }
            swap = *up;
            *up = *down;
            *down = swap;
            uint32_t* nextLeft = leftEqual;
            if (0 <= compare(*up, *pivot, context) && 0 <= compare(*pivot, *up, context)) {
                nextLeft = leftEqual + 1;
                swap = *leftEqual;
                *leftEqual = *up;
                *up = swap;
            }
            if (0 <= compare(*pivot, *down, context) && 0 <= compare(*down, *pivot, context)) {
                rightEqual--;
                swap = *down;
                *down = *rightEqual;
                *rightEqual = swap;
            }
            up++;
            leftEqual = nextLeft;
        }
        swap = *up;
        *up = *pivot;
        *pivot = swap;
        uint32_t* p = first;
        uint32_t* q = up;
        while (p + 1 < leftEqual) {
            q--;
            swap = *p;
            *p = *q;
            *q = swap;
            p++;
        }
        uint32_t* right = up + 1;
        for (uint32_t* r = last - 2; rightEqual < r; r--) {
            swap = *right;
            *right = *r;
            *r = swap;
            right++;
        }
        ideal >>= 1;
        IntroSort(first, q, ideal, compare, context);
        first = right;
    }
    if (32 < last - first) {
        HeapSort(first, last, compare, context);
        return;
    }
    if (first < last) {
        for (uint32_t* p = first + 1; p != last; p++) {
            uint32_t value = *p;
            uint32_t* q = p;
            while (q != first) {
                if (0 <= compare(value, q[-1], context)) {
                    break;
                }
                *q = q[-1];
                q--;
            }
            *q = value;
        }
    }
}

uint32_t Kind(const M2Element* element) {
    return static_cast<uint32_t>(element->m_sortKey >> 0x1f) & 0xf;
}

}  // namespace

// Sorts element indices by a three-way compare over the elements (FUN_141205120).
// ref: FUN_141205120
void M2SortIndices(M2ELEMENTCOMPARE compare, uint32_t* indices, uint64_t count, const M2Element* elements) {
    IntroSort(indices, indices + count, static_cast<int64_t>(count), compare, elements);
}

// Instance group leaders in order (FUN_1411e3860).
int32_t M2ElementCompareGroup(uint32_t a, uint32_t b, const M2Element* elements) {
    uint32_t left = elements[a].m_38;
    uint32_t right = elements[b].m_38;
    if (left < right) {
        return -1;
    }
    return left > right ? 1 : 0;
}

// Opaque order: the sort key, then the batch key (FUN_1411e3aa0).
int32_t M2ElementCompareOpaque(uint32_t a, uint32_t b, const M2Element* elements) {
    const M2Element* left = &elements[a];
    const M2Element* right = &elements[b];
    bool below;
    if (left->m_sortKey != right->m_sortKey) {
        below = left->m_sortKey < right->m_sortKey;
    } else {
        below = left->m_batchKey < right->m_batchKey;
    }
    return below ? -1 : 1;
}

// Two batch elements: by file, flag 4, model, section level, material layer and pipeline for
// kind 2, then the material's blend and flags, its textures, and the batch (FUN_1411e3ae0).
// ref: FUN_1411e3ae0
int32_t M2ElementCompareBatches(const M2Element* left, const M2Element* right) {
    CM2BaseModel* leftModel = left->m_model;
    CM2BaseModel* rightModel = right->m_model;
    if (leftModel->m_type != 0 || rightModel->m_type != 0) {
        auto leftBatch = reinterpret_cast<uintptr_t>(left->m_batch);
        auto rightBatch = reinterpret_cast<uintptr_t>(right->m_batch);
        if (rightBatch <= leftBatch) {
            return leftBatch != rightBatch ? 1 : 0;
        }
        return -1;
    }
    auto leftM2 = static_cast<CM2Model*>(leftModel);
    auto rightM2 = static_cast<CM2Model*>(rightModel);
    CM2Shared* leftShared = leftM2->m_shared;
    CM2Shared* rightShared = rightM2->m_shared;
    auto leftBatch = static_cast<const M2Batch*>(left->m_batch);
    auto rightBatch = static_cast<const M2Batch*>(right->m_batch);
    M2Data* leftData = leftShared->m_data;
    M2Data* rightData = rightShared->m_data;
    if (Kind(left) == 2) {
        if (leftShared != rightShared) {
            return leftShared < rightShared ? -1 : 1;
        }
        uint32_t leftFlag = left->m_flags & 4;
        uint32_t rightFlag = right->m_flags & 4;
        if (leftFlag != rightFlag) {
            return leftFlag < rightFlag ? -1 : 1;
        }
        if (leftModel != rightModel) {
            return leftModel < rightModel ? -1 : 1;
        }
        uint16_t leftLevel = *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(left->m_section) + 0xe);
        uint16_t rightLevel = *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(right->m_section) + 0xe);
        if (leftLevel != rightLevel) {
            return leftLevel < rightLevel ? -1 : 1;
        }
        if (leftBatch->m_materialLayer != rightBatch->m_materialLayer) {
            return leftBatch->m_materialLayer < rightBatch->m_materialLayer ? -1 : 1;
        }
        if (left->m_pipeline != right->m_pipeline) {
            return left->m_pipeline < right->m_pipeline ? -1 : 1;
        }
    }
    const M2Material* leftMaterial = &leftData->m_materials[leftBatch->m_materialIndex];
    const M2Material* rightMaterial = &rightData->m_materials[rightBatch->m_materialIndex];
    if (leftMaterial->m_blendMode < rightMaterial->m_blendMode) {
        return -1;
    }
    if (leftMaterial->m_blendMode != rightMaterial->m_blendMode) {
        return 1;
    }
    uint16_t leftFlags = leftMaterial->m_flags & 0x1f;
    uint16_t rightFlags = rightMaterial->m_flags & 0x1f;
    if (leftFlags < rightFlags) {
        return -1;
    }
    if (leftFlags != rightFlags) {
        return 1;
    }
    uint16_t leftCount = leftBatch->m_textureCount;
    uint16_t rightCount = rightBatch->m_textureCount;
    uint16_t shared = rightCount <= leftCount ? rightCount : leftCount;
    for (uint32_t i = 0; i < shared; i++) {
        uint16_t leftIndex = leftData->m_textureCombos[leftBatch->m_textureComboIndex + i];
        uintptr_t leftTexture = 0;
        if (leftIndex < leftData->m_textures.Count()) {
            leftTexture = reinterpret_cast<uintptr_t>(leftM2->m_textures[leftIndex]);
        }
        uint16_t rightIndex = rightData->m_textureCombos[rightBatch->m_textureComboIndex + i];
        uintptr_t rightTexture = 0;
        if (rightIndex < rightData->m_textures.Count()) {
            rightTexture = reinterpret_cast<uintptr_t>(rightM2->m_textures[rightIndex]);
            if (leftTexture < rightTexture) {
                return -1;
            }
        }
        if (rightTexture < leftTexture) {
            return 1;
        }
    }
    rightCount = rightBatch->m_textureCount;
    if (leftCount < rightCount) {
        return -1;
    }
    if (leftCount != rightCount) {
        return 1;
    }
    if (leftBatch < rightBatch) {
        return -1;
    }
    return leftBatch == rightBatch ? 0 : 1;
}

// The last tie-break between translucent elements: their kind, then batch, emitter or ribbon
// textures (FUN_1411e3d50).
// ref: FUN_1411e3d50
int32_t M2ElementCompareLast(uint32_t a, uint32_t b, const M2Element* elements) {
    const M2Element* left = &elements[a];
    const M2Element* right = &elements[b];
    uint32_t rightKind = Kind(right);
    uint32_t leftKind = Kind(left);
    if (leftKind < rightKind) {
        return -1;
    }
    if (rightKind < leftKind) {
        return 1;
    }
    if (leftKind == 0 || leftKind == 2) {
        return M2ElementCompareBatches(left, right);
    }
    if (leftKind != 4) {
        if (leftKind != 5) {
            return 0;
        }
        return ParticleEmitterCompare(left->m_emitter, right->m_emitter);
    }
    auto leftModel = static_cast<CM2Model*>(left->m_model->m_type == 0 ? left->m_model : nullptr);
    auto rightModel = static_cast<CM2Model*>(right->m_model->m_type == 0 ? right->m_model : nullptr);
    uint32_t rightRibbon = right->m_batchIndex;
    uint32_t leftRibbon = left->m_batchIndex;
    const M2Ribbon* leftData = &leftModel->m_shared->m_data->m_ribbons[leftRibbon];
    const M2Ribbon* rightData = &rightModel->m_shared->m_data->m_ribbons[rightRibbon];
    uint16_t leftCount = static_cast<uint16_t>(leftData->m_textureIndices.Count());
    uint16_t rightCount = static_cast<uint16_t>(rightData->m_textureIndices.Count());
    uint16_t shared = rightCount <= leftCount ? rightCount : leftCount;
    for (uint32_t i = 0; i < shared; i++) {
        auto leftTexture = reinterpret_cast<uintptr_t>(leftModel->m_textures[leftData->m_textureIndices[i]]);
        auto rightTexture = reinterpret_cast<uintptr_t>(rightModel->m_textures[rightData->m_textureIndices[i]]);
        if (leftTexture < rightTexture) {
            return -1;
        }
        if (leftTexture != rightTexture) {
            return 1;
        }
    }
    if (rightCount <= leftCount) {
        if (leftCount != rightCount) {
            return 1;
        }
        if (rightRibbon <= leftRibbon) {
            return rightRibbon < leftRibbon ? 1 : 0;
        }
    }
    return -1;
}

// Translucent order: the non-kind-0 elements first, then screen size, priority plane and
// distance far to near, then the faded flag, the effect kind, and the model, pipeline,
// material layer and the last tie-break (FUN_1411e3f40).
// ref: FUN_1411e3f40
int32_t M2ElementCompareTranslucent(uint32_t a, uint32_t b, const M2Element* elements) {
    const M2Element* left = &elements[a];
    const M2Element* right = &elements[b];
    if (Kind(left) == 0) {
        if (Kind(right) != 0) {
            return -1;
        }
    } else if (Kind(right) == 0 && Kind(left) != 0) {
        return 1;
    }
    float leftSize = left->m_screenSize;
    bool notSmaller = right->m_screenSize <= leftSize;
    if (notSmaller && leftSize != right->m_screenSize) {
        return -1;
    }
    if (!notSmaller) {
        return 1;
    }
    auto leftPlane = static_cast<int32_t>(left->m_3c);
    auto rightPlane = static_cast<int32_t>(right->m_3c);
    if (leftPlane < rightPlane) {
        return -1;
    }
    if (rightPlane < leftPlane) {
        return 1;
    }
    float leftDistance = left->m_distance;
    notSmaller = right->m_distance <= leftDistance;
    if (notSmaller && leftDistance != right->m_distance) {
        return -1;
    }
    if (!notSmaller) {
        return 1;
    }
    uint32_t rightFaded = right->m_flags & 1;
    uint32_t leftFaded = left->m_flags & 1;
    if (rightFaded < leftFaded) {
        return -1;
    }
    if (leftFaded < rightFaded) {
        return 1;
    }
    auto rightEffect = static_cast<int32_t>(right->m_64);
    auto leftEffect = static_cast<int32_t>(left->m_64);
    if (rightEffect < leftEffect) {
        return -1;
    }
    if (leftEffect < rightEffect) {
        return 1;
    }
    uint64_t rightKey = right->m_sortKey;
    uint64_t leftKey = left->m_sortKey;
    if ((((static_cast<uint32_t>(leftKey >> 0x1f) ^ static_cast<uint32_t>(rightKey >> 0x1f)) & 0xf) != 0)
        || left->m_model != right->m_model) {
        if (left->m_pipeline < right->m_pipeline) {
            return -1;
        }
        if (left->m_pipeline != right->m_pipeline) {
            return 1;
        }
        if (left->m_model < right->m_model) {
            return -1;
        }
        if (left->m_model != right->m_model) {
            return 1;
        }
    }
    uint32_t leftKind = static_cast<uint32_t>(leftKey >> 0x1f) & 0xf;
    uint32_t rightKind = static_cast<uint32_t>(rightKey >> 0x1f) & 0xf;
    if (leftKind < rightKind) {
        return -1;
    }
    if (rightKind < leftKind) {
        return 1;
    }
    if (leftKind < 4) {
        uint16_t leftLayer = static_cast<const M2Batch*>(left->m_batch)->m_materialLayer;
        uint16_t rightLayer = static_cast<const M2Batch*>(right->m_batch)->m_materialLayer;
        if (leftLayer < rightLayer) {
            return -1;
        }
        if (leftLayer != rightLayer) {
            return 1;
        }
    }
    if (left->m_pipeline < right->m_pipeline) {
        return -1;
    }
    if (left->m_pipeline == right->m_pipeline) {
        return M2ElementCompareLast(a, b, elements);
    }
    return 1;
}

// Particle runs: the run number, then emitters before others and emitters among themselves,
// then the translucent order (FUN_1411e37e0).
// ref: FUN_1411e37e0
int32_t M2ElementCompareParticleRuns(uint32_t a, uint32_t b, const M2Element* elements) {
    const M2Element* left = &elements[a];
    const M2Element* right = &elements[b];
    auto leftRun = static_cast<uint32_t>(left->m_60);
    auto rightRun = static_cast<uint32_t>(right->m_60);
    if (rightRun < leftRun) {
        return 1;
    }
    if (leftRun < rightRun) {
        return -1;
    }
    uint32_t leftKind = Kind(left);
    if (leftKind == 5 || Kind(right) == 5) {
        uint32_t rightKind = Kind(right);
        if (leftKind > rightKind) {
            return -1;
        }
        if (leftKind < rightKind) {
            return 1;
        }
        if (leftKind == 5) {
            return ParticleEmitterCompare(left->m_emitter, right->m_emitter);
        }
    }
    return M2ElementCompareTranslucent(a, b, elements);
}

// Whether two elements can draw as one instance: same LOD, flag 0x8000, kind, file, batch,
// lighting and colours (FUN_1411de1b0).
// ref: FUN_1411de1b0
bool M2ElementInstanceCompatible(const M2Element* left, const M2Element* right) {
    CM2BaseModel* rightModel = right->m_model;
    CM2BaseModel* leftModel = left->m_model;
    if (leftModel->m_lod != rightModel->m_lod || ((left->m_flags ^ right->m_flags) >> 0xf & 1) != 0) {
        return false;
    }
    if (leftModel->m_type != rightModel->m_type) {
        return false;
    }
    if (leftModel->m_type == 0) {
        auto leftM2 = static_cast<CM2Model*>(leftModel);
        auto rightM2 = static_cast<CM2Model*>(rightModel);
        if (leftM2->m_shared != rightM2->m_shared || left->m_batchIndex != right->m_batchIndex) {
            return false;
        }
        auto leftLight = reinterpret_cast<const uint8_t*>(leftModel->m_lightingUsed);
        auto rightLight = reinterpret_cast<const uint8_t*>(rightModel->m_lightingUsed);
        if (leftLight != rightLight) {
            auto f = [&](uint32_t offset) {
                return *reinterpret_cast<const float*>(rightLight + offset) != *reinterpret_cast<const float*>(leftLight + offset);
            };
            if (*reinterpret_cast<const uint32_t*>(leftLight + 0x104) != *reinterpret_cast<const uint32_t*>(rightLight + 0x104)
                || f(0x7c) || f(0x80) || f(0x84) || f(0x58) || f(0x5c) || f(0x60) || f(0x94) || f(0x98) || f(0x9c)) {
                return false;
            }
        }
        uint16_t color = static_cast<const M2Batch*>(left->m_batch)->m_colorIndex;
        if (color < leftM2->m_shared->m_data->m_colors.Count()) {
            auto leftColor = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(leftM2->m_colors) + color * 0x20);
            auto rightColor = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(rightM2->m_colors) + color * 0x20);
            if (rightColor[2] != leftColor[2] || rightColor[3] != leftColor[3] || rightColor[4] != leftColor[4]) {
                return false;
            }
        }
    } else if (left->m_batchIndex != right->m_batchIndex) {
        return false;
    }
    auto leftBytes = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(leftModel) + 0x368);
    auto rightBytes = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(rightModel) + 0x368);
    for (uint32_t i = 0; i < 6; i++) {
        if (rightBytes[i] != leftBytes[i]) {
            return false;
        }
    }
    return true;
}

// Additive and alpha-key emitters in a pass split into runs of consecutive elements, each run
// numbered on the element, then the pass sorted by run (FUN_1411e35f0).
// ref: FUN_1411e35f0
void M2SceneParticleRuns(CM2Scene* scene, uint32_t pass) {
    M2SceneVector<uint32_t>* indices = &scene->m_f90[pass];
    auto elements = reinterpret_cast<M2Element*>(scene->m_f78.m_data);
    uint32_t count = static_cast<uint32_t>(indices->m_size);
    uint32_t run = 0;
    bool inRun = false;
    static const int32_t s_effectBlend[8] = { 2, 2, 2, 3, 4, 5, 6, 7 };  // DAT_142721390
    for (uint32_t i = 0; i < count; i++) {
        M2Element* element = &elements[indices->m_data[i]];
        uint32_t blend = 0;
        CM2Model* model = element->m_model->m_type == 0 ? static_cast<CM2Model*>(element->m_model) : nullptr;
        if (model != nullptr) {
            M2Data* data = model->m_shared->m_data;
            uint32_t kind = Kind(element);
            if (kind < 8) {
                switch (kind) {
                case 1:
                case 6:
                case 7:
                    break;
                case 4: {
                    const M2Ribbon* ribbon = &data->m_ribbons[element->m_batchIndex];
                    blend = data->m_materials[ribbon->m_materialIndices[0]].m_blendMode;
                    break;
                }
                case 5: {
                    uint8_t mode = 3;
                    if ((element->m_emitter->m_flags & 0x4000000) == 0) {
                        mode = element->m_emitter->m_blend;
                    }
                    blend = M2BlendModeIndex(mode);
                    break;
                }
                default:
                    blend = data->m_materials[static_cast<const M2Batch*>(element->m_batch)->m_materialIndex].m_blendMode;
                    break;
                }
            }
        }
        bool joins;
        if (static_cast<uint32_t>(s_effectBlend[blend] - 3) < 2) {
            joins = inRun;
            inRun = true;
        } else {
            joins = false;
            inRun = false;
        }
        if (!joins) {
            run++;
        }
        element->m_60 = static_cast<int32_t>(run);
    }
    M2SortIndices(M2ElementCompareParticleRuns, indices->m_data, indices->m_size, elements);
}

// The passes sorted: 0-7 but 3 in the opaque order unless `half` is 2, 7-13 but 11 in the
// translucent order unless `half` is odd; then the particle runs of passes 1, 8 and 10 when the
// cache asks and more than one such emitter drew (FUN_1411e3890).
// ref: FUN_1411e3890
void CM2Scene::SortPasses(uint32_t half) {
    auto elements = reinterpret_cast<const M2Element*>(this->m_f78.m_data);
    if (this->m_f78.m_size != 0) {
        if (static_cast<int32_t>(half) < 2) {
            static const uint32_t s_opaque[6] = { 0, 1, 2, 4, 5, 6 };
            for (uint32_t pass : s_opaque) {
                M2SortIndices(M2ElementCompareOpaque, this->m_f90[pass].m_data, this->m_f90[pass].m_size, elements);
            }
        }
        if ((half & 1) == 0) {
            M2SortIndices(M2ElementCompareOpaque, this->m_f90[7].m_data, this->m_f90[7].m_size, elements);
            static const uint32_t s_translucent[5] = { 8, 9, 10, 12, 13 };
            for (uint32_t pass : s_translucent) {
                M2SortIndices(M2ElementCompareTranslucent, this->m_f90[pass].m_data, this->m_f90[pass].m_size, elements);
            }
        }
    }
    if ((this->m_cache->m_flags & 0x40) != 0 && 1 < static_cast<uint32_t>(this->m_db0c) && static_cast<int32_t>(half) < 2) {
        M2SceneParticleRuns(this, 1);
    }
    if ((this->m_cache->m_flags & 0x40) != 0 && 1 < static_cast<uint32_t>(this->m_db0c) && (half & 1) == 0) {
        M2SceneParticleRuns(this, 8);
        M2SceneParticleRuns(this, 10);
    }
    this->m_db0c = 0;
}

namespace {

// Appends a run of indices (FUN_1411e4be0 opening room at the end, then the copy).
void AppendIndices(M2SceneVector<uint32_t>* indices, const uint32_t* source, uint64_t count) {
    uint64_t size = indices->m_size;
    if ((indices->m_capacity & 0x7fffffffffffffffull) < size + count) {
        M2IndicesReserve(indices, size + count);
    }
    for (uint64_t i = 0; i < count; i++) {
        indices->m_data[size + i] = source[i];
    }
    indices->m_size = size + count;
}

// Appends elements (FUN_1411cf4f0 and FUN_1411e4db0).
void AppendElements(M2SceneVector<M2Element>* elements, const M2Element* source, uint64_t count) {
    uint64_t size = elements->m_size;
    if ((elements->m_capacity & 0x7fffffffffffffffull) < size + count) {
        M2ElementsReserve(elements, size + count);
    }
    if (count != 0) {
        std::memcpy(&elements->m_data[size], source, count * sizeof(M2Element));
    }
    elements->m_size = size + count;
}

// Resizes an index vector, zeroing what it adds.
void ResizeIndices(M2SceneVector<uint32_t>* indices, uint64_t count) {
    if (count < indices->m_size) {
        indices->m_size = count;
    } else if (indices->m_size < count) {
        M2IndicesReserve(indices, count);
        std::memset(&indices->m_data[indices->m_size], 0, (count - indices->m_size) * 4);
        indices->m_size = count;
    }
}

// A pass's instanced elements grouped: each one's leader found through an open-addressed
// table by its light key, the indices sorted by leader, and each run's length stored at its
// start.
void GroupInstances(CM2Scene* scene, uint32_t pass) {
    M2SceneVector<uint32_t>* instanced = &scene->m_10e0[pass];
    M2SceneVector<uint32_t>* runs = &scene->m_1230[pass];
    M2Element* elements = scene->m_f78.m_data;
    uint32_t count = static_cast<uint32_t>(instanced->m_size);
    ResizeIndices(runs, count);
    uint64_t out = 0;
    if (count != 0) {
        if (2 <= count) {
            uint32_t tableSize = 0x80;
            uint32_t wanted = count * 3 >> 1;
            if (0x80 < wanted) {
                do {
                    tableSize *= 2;
                } while (tableSize < wanted);
            }
            M2SceneVector<uint32_t>* table = &scene->m_d798[pass];
            if (table->m_size < tableSize) {
                ResizeIndices(table, tableSize);
            }
            std::memset(table->m_data, 0xff, static_cast<uint64_t>(tableSize) * 4);
            for (uint32_t k = 0; k < count; k++) {
                uint32_t index = instanced->m_data[k];
                uint32_t start = elements[index].m_18 & (tableSize - 1);
                uint32_t slot = start;
                uint32_t leader;
                do {
                    slot = slot + 1 < tableSize ? slot + 1 : 0;
                    leader = table->m_data[slot];
                    if (leader == 0xffffffff || slot == start) {
                        table->m_data[slot] = index;
                        leader = index;
                        break;
                    }
                } while (!M2ElementInstanceCompatible(&elements[index], &elements[leader]));
                elements[index].m_38 = leader;
            }
            M2SortIndices(M2ElementCompareGroup, instanced->m_data, count, elements);
        }
        uint32_t k = 0;
        do {
            uint32_t start = k;
            uint32_t leader = instanced->m_data[k];
            k++;
            instanced->m_data[out] = leader;
            uint64_t next = out + 1;
            while (k < count) {
                uint32_t index = instanced->m_data[k];
                if (elements[index].m_38 != elements[leader].m_38) {
                    break;
                }
                instanced->m_data[next] = index;
                next++;
                k++;
            }
            runs->m_data[out] = k - start;
            out = next;
        } while (k < count);
    }
    ResizeIndices(instanced, out);
    ResizeIndices(runs, out);
}

}  // namespace

// ref: FUN_1411de670
void CM2Scene::FinishElements(uint32_t threaded, uint32_t lists, uint32_t which) {
    uint32_t firstPass = 0;
    uint32_t endPass = 0xe;
    uint32_t mergeEnd = 0xe;
    uint32_t half = 0;
    M2SceneDrawList* single = &this->m_drawLists[0x40];
    if (threaded == 0) {
        for (uint32_t p = 0; p < 0xe; p++) {
            this->m_f90[p].m_size = 0;
            this->m_10e0[p].m_size = 0;
            AppendIndices(&this->m_f90[p], single->m_lists[p].m_data, single->m_lists[p].m_size);
            AppendIndices(&this->m_10e0[p], single->m_lists2[p].m_data, single->m_lists2[p].m_size);
        }
        this->m_f78.m_size = 0;
        AppendElements(&this->m_f78, single->m_elements.m_data, single->m_elements.m_size);
    } else {
        uint32_t mergeStart = 0;
        if (which == 2) {
            half = 2;
            mergeStart = 6;
            firstPass = 6;
        } else if (which == 1) {
            endPass = 6;
            half = 1;
            mergeEnd = 6;
        }
        uint64_t sizes[0x40] = {};
        if (which == 2) {
            while (-1 < static_cast<int32_t>(this->m_db18)) {
            }
        } else {
            uint64_t total = single->m_elements.m_size;
            for (uint32_t i = 0; i < lists; i++) {
                sizes[i] = this->m_drawLists[i].m_elements.m_size;
                total += sizes[i];
            }
            for (uint32_t p = 0; p < 0xe; p++) {
                this->m_f90[p].m_size = 0;
                this->m_10e0[p].m_size = 0;
            }
            M2ElementsReserve(&this->m_f78, total);
            this->m_f78.m_size = 0;
            std::memset(this->m_d8e8, 0, sizeof(this->m_d8e8));
            uint64_t offset = single->m_elements.m_size;
            AppendElements(&this->m_f78, single->m_elements.m_data, single->m_elements.m_size);
            for (uint32_t i = 0; i < lists; i++) {
                this->m_d8e8[i] = offset;
                AppendElements(&this->m_f78, this->m_drawLists[i].m_elements.m_data, this->m_drawLists[i].m_elements.m_size);
                offset += sizes[i];
            }
            InterlockedDecrement(reinterpret_cast<volatile long*>(&this->m_db18));
        }
        for (uint32_t p = mergeStart; p < mergeEnd; p++) {
            AppendIndices(&this->m_f90[p], single->m_lists[p].m_data, single->m_lists[p].m_size);
            AppendIndices(&this->m_10e0[p], single->m_lists2[p].m_data, single->m_lists2[p].m_size);
        }
        for (uint32_t i = 0; i < lists; i++) {
            M2SceneDrawList* list = &this->m_drawLists[i];
            auto offset = static_cast<int32_t>(this->m_d8e8[i]);
            for (uint32_t p = mergeStart; p < mergeEnd; p++) {
                for (uint64_t k = 0; k < list->m_lists[p].m_size; k++) {
                    M2IndicesPush(&this->m_f90[p], static_cast<uint32_t>(static_cast<int32_t>(list->m_lists[p].m_data[k]) + offset));
                }
                for (uint64_t k = 0; k < list->m_lists2[p].m_size; k++) {
                    M2IndicesPush(&this->m_10e0[p], static_cast<uint32_t>(static_cast<int32_t>(list->m_lists2[p].m_data[k]) + offset));
                }
            }
        }
        if (endPass <= firstPass) {
            this->SortPasses(half);
            return;
        }
    }
    for (uint32_t p = firstPass; p < endPass; p++) {
        GroupInstances(this, p);
    }
    this->SortPasses(half);
}
