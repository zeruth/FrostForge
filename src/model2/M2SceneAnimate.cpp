#include "model2/M2Scene.hpp"

#include <cstring>
#include <new>

#include "blz/Function.hpp"
#include "client/RenderSettings.hpp"
#include "model2/CM2Lighting.hpp"
#include "model2/M2Shared.hpp"
#include "storm/ErrorReport.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "threadpool/ThreadPool.hpp"
#include "util/Unimplemented.hpp"

// Engine/Source/Model2/M2Scene.cpp: a frame's animation. Every model sits in one of the
// scene's 64 buckets; each bucket's draw list collects its models' callbacks, animates them
// and lights them, on the thread pool for the world scene and on this thread otherwise.

namespace {

// DAT_142da1ce0, DAT_142d61348, DAT_142da2180.
const char s_callbackName[] = ".?AUM2ModelCallback@@";
const char s_indexName[] = ".?I";
const char s_elementName[] = ".?AVM2Element@@";

// An error-report record naming what a list was doing to which model (LAB_1411cf3c0).
struct MODELCONTEXT {
    ERRORCONTEXT m_base;
    const char* m_format;               // 0x10
    int32_t m_fileDataId;               // 0x18
};

void DescribeModel(ERRORCONTEXT* base, uint64_t size, char* buffer) {
    auto context = reinterpret_cast<MODELCONTEXT*>(base);
    SStrPrintf(buffer, static_cast<uint32_t>(size), context->m_format, context->m_fileDataId);
}

// A crash-report record carrying a model's memory (LAB_1411ace50).
struct MODELMEMORY {
    ERRORMEMORYCONTEXT m_base;
    CM2BaseModel* m_model;              // 0x10
};

void ReportModelMemory(ERRORMEMORYCONTEXT* base, ERRORMEMORYREPORT report) {
    report(reinterpret_cast<MODELMEMORY*>(base)->m_model, 0x688);
}

// A pool task running `function` on a list (the invoker LAB_140601040 over FUN_140601070).
struct LISTTASK {
    void (*m_function)(M2SceneDrawList* list);
    M2SceneDrawList* m_list;
};
static_assert(sizeof(LISTTASK) <= 16, "a list task fits the function storage");

void InvokeListTask(void* storage) {
    auto task = static_cast<LISTTASK*>(storage);
    task->m_function(task->m_list);
}

void SetListTask(blz::function_storage* storage, void (*function)(M2SceneDrawList* list), M2SceneDrawList* list) {
    storage->m_manager(storage->m_storage, nullptr, 0);
    storage->m_invoke = reinterpret_cast<void*>(&InvokeListTask);
    storage->m_manager = blz::function_trivial_manager;
    auto task = reinterpret_cast<LISTTASK*>(storage->m_storage);
    task->m_list = list;
    task->m_function = function;
}

void ClearTasks(blz::function_storage* tasks) {
    for (uint32_t i = 0; i < 0x40; i++) {
        tasks[i].m_invoke = nullptr;
        tasks[i].m_manager = blz::function_empty_manager;
    }
}

void DestroyTasks(blz::function_storage* tasks) {
    for (uint32_t i = 0x40; i != 0; i--) {
        tasks[i - 1].m_manager(tasks[i - 1].m_storage, nullptr, 0);
    }
}

// Runs a list's callbacks and empties them (the loop FUN_1411d74a0 and FUN_1411d2180 inline).
void RunListCallbacks(M2SceneDrawList* list) {
    M2ModelCallback* callback = list->m_callbacks.m_data;
    M2ModelCallback* end = callback + list->m_callbacks.m_size;
    for (; callback != end; callback++) {
        CM2Model* model = callback->m_model;
        if (0 < model->m_refCount) {
            model->RunCallback(callback);
        }
    }
    list->m_callbacks.m_size = 0;
}

// Grows `rect` to cover `other` when both are rectangles; takes `other` when `rect` is empty.
void MergeRect(float* rect, const float* other, bool checkOther) {
    if (rect[2] <= rect[0] || rect[3] <= rect[1]) {
        rect[0] = other[0];
        rect[1] = other[1];
        rect[2] = other[2];
        rect[3] = other[3];
        return;
    }
    if (checkOther && !(other[0] < other[2] && other[1] < other[3])) {
        return;
    }
    float x1 = rect[2] < other[2] ? other[2] : rect[2];
    float y1 = rect[3] < other[3] ? other[3] : rect[3];
    float x0 = other[0] < rect[0] ? other[0] : rect[0];
    float y0 = other[1] < rect[1] ? other[1] : rect[1];
    rect[0] = x0;
    rect[1] = y0;
    rect[2] = x1;
    rect[3] = y1;
}

// A list on the stack for one bucket, as the single-threaded passes build it.
void StackListInit(M2SceneDrawList* list, CM2Scene* scene, uint32_t index) {
    list->m_flags = 0;
    list->m_scene = scene;
    list->m_index = index;
}

}  // namespace

// ref: FUN_1411acff0
void M2ModelParamsInit(CM2ModelParams* params) {
    params->m_flags &= 0xfffffffe;
    params->m_effect = nullptr;
    params->m_matrix = nullptr;
    params->m_alpha = 1.0f;
    params->m_colorAlpha = 1.0f;
    params->m_fade = 1.0f;
    params->m_color = { 1.0f, 1.0f, 1.0f };
    params->m_colorAdd = { 0.0f, 0.0f, 0.0f };
}

// ref: FUN_1411e5770
void M2ModelCallbacksResize(M2SceneVector<M2ModelCallback>* callbacks, uint64_t count) {
    uint64_t size = callbacks->m_size;
    if (count < size) {
        callbacks->m_size = count;
        return;
    }
    if (size < count) {
        uint64_t field = callbacks->m_capacity;
        if ((field & 0x7fffffffffffffffull) < count) {
            uint64_t capacity = (field >> 1 & 0x3fffffffffffffffull) + (field & 0x7fffffffffffffffull);
            if (capacity < count) {
                capacity = count;
            }
            callbacks->m_capacity = ((field ^ capacity) & 0x8000000000000000ull) ^ capacity;
            auto data = static_cast<M2ModelCallback*>(SMemAlloc(capacity * sizeof(M2ModelCallback), 0xb, s_callbackName,
                SStrHashFnv1a(s_callbackName), -6, 0));
            size = callbacks->m_size;
            if (size != 0) {
                std::memcpy(data, callbacks->m_data, size * sizeof(M2ModelCallback));
            }
            if (static_cast<int64_t>(callbacks->m_capacity) >= 0) {
                SMemFree(callbacks->m_data, s_callbackName, -6, 0);
                size = callbacks->m_size;
            }
            callbacks->m_capacity &= 0x7fffffffffffffffull;
            callbacks->m_data = data;
        }
        for (uint64_t i = size; i != count; i++) {
            std::memset(&callbacks->m_data[i], 0, sizeof(M2ModelCallback));
        }
        callbacks->m_size = count;
    }
}

// ref: FUN_1411d75f0
void M2SceneDrawListReset(M2SceneDrawList* list) {
    uint32_t flags = list->m_flags & 0xfffffffc;
    list->m_flags = flags;
    if (list->m_elements.m_size != 0) {
        list->m_elements.m_size = 0;
        flags = list->m_flags;
    }
    list->m_elementCount = 0;
    list->m_index = 0;
    list->m_flags = flags & 0xffffffe3;
    list->m_2f4 = 0;
    std::memset(list->m_screenRect, 0, sizeof(list->m_screenRect));
    for (uint32_t i = 0; i < 0xe; i++) {
        list->m_lists[i].m_size = 0;
        list->m_lists2[i].m_size = 0;
    }
    M2ModelCallbacksResize(&list->m_callbacks, 0);
}

// ref: FUN_1411d0cb0
void M2SceneDrawListFree(M2SceneDrawList* list) {
    if (static_cast<int64_t>(list->m_callbacks.m_capacity) >= 0) {
        SMemFree(list->m_callbacks.m_data, s_callbackName, -6, 0);
        list->m_callbacks.m_data = nullptr;
    }
    for (uint32_t i = 0xe; i != 0; i--) {
        if (static_cast<int64_t>(list->m_lists2[i - 1].m_capacity) >= 0) {
            SMemFree(list->m_lists2[i - 1].m_data, s_indexName, -6, 0);
            list->m_lists2[i - 1].m_data = nullptr;
        }
    }
    for (uint32_t i = 0xe; i != 0; i--) {
        if (static_cast<int64_t>(list->m_lists[i - 1].m_capacity) >= 0) {
            SMemFree(list->m_lists[i - 1].m_data, s_indexName, -6, 0);
            list->m_lists[i - 1].m_data = nullptr;
        }
    }
    if (static_cast<int64_t>(list->m_elements.m_capacity) >= 0) {
        SMemFree(list->m_elements.m_data, s_elementName, -6, 0);
        list->m_elements.m_data = nullptr;
    }
}

// ref: FUN_1411dd760
void M2SceneDrawListCollectCallbacks(M2SceneDrawList* list) {
    CM2BaseModel* model = list->m_scene->m_buckets[list->m_index];
    while (model != nullptr) {
        MODELCONTEXT context;
        context.m_fileDataId = model->GetFileDataId();
        ErrorContextPush(&context.m_base, DescribeModel);
        context.m_format = "M2 Generate Animation Callbacks: model=%d";
        MODELMEMORY memory;
        ErrorMemoryContextPush(&memory.m_base, ReportModelMemory);
        CM2BaseModel* next = model->m_bucketNext;
        memory.m_model = model;
        // The callbacks are gathered into a vector on the stack with room for 0x400.
        M2ModelCallback storage[0x400];
        M2SceneVector<M2ModelCallback> callbacks;
        callbacks.m_data = storage;
        callbacks.m_size = 0;
        callbacks.m_capacity = 0x8000000000000400ull;
        if (model->m_parent == nullptr) {
            model->CollectEvents(list, &callbacks);
        }
        if (static_cast<int64_t>(callbacks.m_capacity) >= 0) {
            SMemFree(callbacks.m_data, s_callbackName, -6, 0);
            callbacks.m_data = nullptr;
        }
        ErrorMemoryContextPop(&memory.m_base);
        ErrorContextPop(&context.m_base);
        model = next;
    }
}

// ref: FUN_1411d3410
void M2SceneDrawListAnimate(M2SceneDrawList* list) {
    CM2Scene* scene = list->m_scene;
    uint32_t frame = scene->m_frame;
    CM2BaseModel* model = scene->m_buckets[list->m_index];
    uint32_t time = scene->m_time;
    uint32_t skip = static_cast<uint32_t>(M2AnimFrameSkipLod());
    CM2ModelParams params;
    M2ModelParamsInit(&params);
    for (; model != nullptr; model = model->m_bucketNext) {
        MODELCONTEXT context;
        context.m_fileDataId = model->GetFileDataId();
        ErrorContextPush(&context.m_base, DescribeModel);
        context.m_format = "M2 Animate: model=%d";
        if (model->m_parent == nullptr) {
            if (model->m_type == 0) {
                params.m_effect = static_cast<CM2Model*>(model)->m_effect2;
            }
            model->UpdateMatrices(&scene->m_camera, &params, frame, time, skip);
        }
        ErrorContextPop(&context.m_base);
    }
}

// ref: FUN_1411de480
void M2SceneDrawListPostAnimate(M2SceneDrawList* list) {
    CM2Scene* scene = list->m_scene;
    uint32_t index = list->m_index;
    CM2BaseModel** bucket = &scene->m_buckets[index];
    CM2BaseModel* model = *bucket;
    while (model != nullptr) {
        MODELCONTEXT context;
        context.m_fileDataId = model->GetFileDataId();
        ErrorContextPush(&context.m_base, DescribeModel);
        context.m_format = "M2 Post Animate: model=%d";
        MODELMEMORY memory;
        ErrorMemoryContextPush(&memory.m_base, ReportModelMemory);
        memory.m_model = model;
        if (model->m_parent == nullptr) {
            model->PrepareLighting(static_cast<int32_t>(index), 0);
            if (model->m_type == 0) {
                static_cast<CM2Model*>(model)->UpdateLights();
            }
        }
        if ((list->m_flags & 4) != 0
            && ((model->m_flags >> 0x11 & 1) != 0 || ((model->m_flags >> 0x12 & 1) != 0 && model->m_parent != nullptr))) {
            M2SceneProjectScreenRect(model, &scene->m_camera, &scene->m_d4f8, scene->m_viewportMin, scene->m_viewportMax,
                list->m_screenRect);
        }
        *bucket = model->m_bucketNext;
        CM2BaseModel* next = model->m_bucketNext;
        model->m_flags &= 0xffdfffff;
        model->m_bucketNext = nullptr;
        model->m_bucketLink = nullptr;
        if (model->IsShownInTree() && (model->m_type == 0 || model->m_type == 3)) {
            model->UpdateLighting(&scene->m_camera);
            if (model->m_type == 0) {
                auto m2 = static_cast<CM2Model*>(model);
                if (m2->m_5f0 != 0 && (model->m_flags & CM2BaseModel::BASE_CULLED) == 0) {
                    m2->UpdateProjectedBatches();
                }
            }
        }
        ErrorMemoryContextPop(&memory.m_base);
        ErrorContextPop(&context.m_base);
        model = next;
    }
}

// The rectangle comes out as {min y, min x, max y, max x}, merged into `rect` when that holds
// one; a model not loaded and visible, or with an empty box, clears it.
// ref: FUN_1411d6fc0
void M2SceneProjectScreenRect(CM2BaseModel* model, const C44Matrix* view, const C44Matrix* projection,
    const float* viewportMin, const float* viewportMax, float* rect) {
    float minX = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    float out0 = 0.0f;
    float box[6];
    if ((model->m_flags & 5) == 5 && (model->GetAnimatedBox(box, true),
        !(box[3] < box[0] && box[4] < box[1] && box[5] < box[2]))) {
        float cx = (box[3] + box[0]) * 0.5f;
        float cy = (box[4] + box[1]) * 0.5f;
        float cz = (box[5] + box[2]) * 0.5f;
        float hx = (box[3] - box[0]) * 0.5f;
        float hy = (box[4] - box[1]) * 0.5f;
        float hz = (box[5] - box[2]) * 0.5f;
        float x1 = hx + cx;
        float x0 = cx - hx;
        float y1 = hy + cy;
        float y0 = cy - hy;
        float z1 = hz + cz;
        float z0 = cz - hz;
        const float corners[8][3] = {
            { x1, y1, z1 }, { x0, y1, z1 }, { x1, y0, z1 }, { x0, y0, z1 },
            { x1, y1, z0 }, { x0, y1, z0 }, { x1, y0, z0 }, { x0, y0, z0 },
        };
        const float* v = view->m;
        const float* p = projection->m;
        float originX = viewportMin[0];
        float originY = viewportMin[1];
        float minY = 0.0f;
        bool behind = false;
        for (uint32_t i = 0; i < 8; i++) {
            float x = corners[i][0];
            float y = corners[i][1];
            float z = corners[i][2];
            float vx = y * v[4] + x * v[0] + z * v[8] + v[12];
            float vy = y * v[5] + x * v[1] + z * v[9] + v[13];
            float vz = y * v[6] + x * v[2] + z * v[10] + v[14];
            float vw = y * v[7] + x * v[3] + z * v[11] + v[15];
            float w = vw * p[15] + vz * p[11] + vy * p[7] + vx * p[3];
            if (w <= 0.0f) {
                behind = true;
                break;
            }
            w = 1.0f / w;
            float sx = ((vw * p[12] + vz * p[8] + vy * p[4] + vx * p[0]) * w + 1.0f) * 0.5f * (viewportMax[0] - originX) + originX;
            float sy = ((vw * p[13] + vz * p[9] + vy * p[5] + vx * p[1]) * w + 1.0f) * 0.5f * (viewportMax[1] - originY) + originY;
            if (i == 0) {
                minX = sx;
                maxX = sx;
                minY = sy;
                maxY = sy;
                continue;
            }
            if (sx < minX) {
                minX = sx;
            }
            if (sy < minY) {
                minY = sy;
            }
            if (maxX < sx) {
                maxX = sx;
            }
            if (maxY < sy) {
                maxY = sy;
            }
        }
        if (behind) {
            minX = 0.0f;
            minY = 0.0f;
            maxX = 1.0f;
            maxY = 1.0f;
        }
        float rect0 = rect[0];
        if (minX <= 0.0f) {
            minX = 0.0f;
        }
        float rect2 = rect[2];
        if (minY <= 0.0f) {
            minY = 0.0f;
        }
        if (1.0f <= maxX) {
            maxX = 1.0f;
        }
        if (1.0f <= maxY) {
            maxY = 1.0f;
        }
        out0 = minY;
        if (rect0 < rect2 && rect[1] < rect[3]) {
            if (maxX <= rect[3]) {
                maxX = rect[3];
            }
            float mergedX = rect[1];
            if (minX <= rect[1]) {
                mergedX = minX;
            }
            if (rect2 <= maxY) {
                rect2 = maxY;
            }
            maxY = rect2;
            out0 = rect0;
            minX = mergedX;
            if (minY <= rect0) {
                out0 = minY;
            }
        }
    }
    rect[0] = out0;
    rect[1] = minX;
    rect[2] = maxY;
    rect[3] = maxX;
}

// ref: FUN_1411d74a0
void CM2Scene::RunCallbacks(M2SceneDrawList* list) {
    this->m_flags |= 4;
    if (this->m_timeStep == 0) {
        this->m_flags &= 0xfffffffb;
        return;
    }
    if (list == nullptr) {
        for (uint32_t i = 0; i < 0x40; i++) {
            RunListCallbacks(&this->m_drawLists[i]);
        }
    } else {
        RunListCallbacks(list);
    }
    this->m_flags &= 0xfffffffb;
}

// ref: FUN_1411d2150
void CM2Scene::Animate(void* frustum, bool threaded) {
    this->m_frustum = frustum;
    this->Animate(threaded);
    this->m_frustum = nullptr;
}

// ref: FUN_1411d2180
void CM2Scene::Animate(bool threaded) {
    bool bucketModels = this->m_bucketModels;
    for (CM2Lighting* lighting = this->m_pointLights; lighting != nullptr;) {
        CM2Lighting* next = lighting->m_next;
        if (lighting->m_scene != nullptr && lighting->m_08 != this->m_frame) {
            lighting->SetEnabled(0);
        }
        lighting = next;
    }
    bool trackRects = g_renderSettings.m_modelScreenRects != 0;
    if (trackRects) {
        std::memset(this->m_screenRect, 0, sizeof(this->m_screenRect));
    }
    if ((this->m_cache->m_flags & 4) == 0 || !(threaded && bucketModels)) {
        uint32_t count = this->m_bucketModels ? 0x40 : 1;
        for (uint32_t i = 0; i < count; i++) {
            if (this->m_buckets[i] == nullptr) {
                continue;
            }
            M2SceneDrawList* list = &this->m_drawLists[i];
            M2SceneDrawListReset(list);
            list->m_scene = this;
            list->m_index = i;
            std::memset(list->m_screenRect, 0, sizeof(list->m_screenRect));
            list->m_flags = static_cast<uint32_t>(trackRects) << 2 | (list->m_flags & 0xffffffe3);
            M2SceneDrawListCollectCallbacks(list);
            this->RunCallbacks(list);
        }
        uint32_t again;
        do {
            again = 0;
            for (uint32_t i = 0; i < count; i++) {
                if (this->m_buckets[i] == nullptr) {
                    continue;
                }
                M2SceneDrawList* list = &this->m_drawLists[i];
                if ((list->m_flags & 8) != 0) {
                    list->m_flags = (list->m_flags & 0xfffffff7) | 0x10;
                    M2SceneDrawListCollectCallbacks(list);
                    this->RunCallbacks(list);
                    again++;
                }
            }
        } while (again != 0);
        for (uint32_t i = 0; i < count; i++) {
            if (this->m_buckets[i] == nullptr) {
                continue;
            }
            M2SceneDrawList list;
            StackListInit(&list, this, i);
            M2SceneDrawListAnimate(&list);
            M2SceneDrawListFree(&list);
        }
        for (uint32_t i = 0; i < count; i++) {
            if (this->m_buckets[i] == nullptr) {
                continue;
            }
            M2SceneDrawList list;
            StackListInit(&list, this, i);
            list.m_flags = static_cast<uint32_t>(trackRects) << 2 | (list.m_flags & 0xfffffffb);
            M2SceneDrawListPostAnimate(&list);
            if (trackRects && this->m_bucketModels) {
                MergeRect(this->m_screenRect, list.m_screenRect, false);
            }
            M2SceneDrawListFree(&list);
        }
    } else {
        blz::function_storage tasks[0x40];
        ClearTasks(tasks);
        ThreadPoolGroupSetCount(static_cast<THREADPOOLGROUP*>(this->m_threadGroupB), 0x3f);
        for (uint32_t i = 0; i < 0x3f; i++) {
            M2SceneDrawList* list = &this->m_drawLists[i];
            list->m_scene = this;
            list->m_index = i;
            std::memset(list->m_screenRect, 0, sizeof(list->m_screenRect));
            list->m_flags = (list->m_flags & 0xffffffe3) | static_cast<uint32_t>(trackRects) * 4;
            SetListTask(&tasks[i], M2SceneDrawListCollectCallbacks, list);
        }
        ThreadPoolSubmit(0x3f, tasks, 2, static_cast<THREADPOOLGROUP*>(this->m_threadGroupB));
        M2SceneDrawList* last = &this->m_drawLists[0x3f];
        std::memset(last->m_screenRect, 0, sizeof(last->m_screenRect));
        last->m_scene = this;
        last->m_index = 0x3f;
        last->m_flags = (last->m_flags & 0xffffffe3) | static_cast<uint32_t>(trackRects) * 4;
        M2SceneDrawListCollectCallbacks(last);
        ThreadPoolGroupWait(static_cast<THREADPOOLGROUP*>(this->m_threadGroupB));
        this->RunCallbacks(nullptr);
        int32_t again;
        do {
            blz::function_storage more[0x40];
            ClearTasks(more);
            again = 0;
            for (uint32_t i = 0; i < 0x3f; i++) {
                M2SceneDrawList* list = &this->m_drawLists[i];
                if ((list->m_flags & 8) != 0) {
                    list->m_flags = (list->m_flags & 0xfffffff7) | 0x10;
                    SetListTask(&more[again], M2SceneDrawListCollectCallbacks, list);
                    again++;
                }
            }
            if (again != 0) {
                ThreadPoolGroupSetCount(static_cast<THREADPOOLGROUP*>(this->m_threadGroupB), again);
                ThreadPoolSubmit(static_cast<uint64_t>(again), more, 2, static_cast<THREADPOOLGROUP*>(this->m_threadGroupB));
            }
            if ((last->m_flags & 8) != 0) {
                again++;
                last->m_flags = (last->m_flags & 0xfffffff7) | 0x10;
                M2SceneDrawListCollectCallbacks(last);
            }
            if (again != 0) {
                ThreadPoolGroupWait(static_cast<THREADPOOLGROUP*>(this->m_threadGroupB));
                this->m_flags |= 4;
                if (this->m_timeStep != 0) {
                    for (uint32_t i = 0; i < 0x40; i++) {
                        RunListCallbacks(&this->m_drawLists[i]);
                    }
                }
                this->m_flags &= 0xfffffffb;
            }
            DestroyTasks(more);
        } while (again != 0);
        ThreadPoolGroupSetCount(static_cast<THREADPOOLGROUP*>(this->m_threadGroupB), 0x3f);
        for (uint32_t i = 0; i < 0x3f; i++) {
            M2SceneDrawList* list = &this->m_drawLists[i];
            list->m_scene = this;
            list->m_index = i;
            SetListTask(&tasks[i], M2SceneDrawListAnimate, list);
        }
        ThreadPoolSubmit(0x3f, tasks, 2, static_cast<THREADPOOLGROUP*>(this->m_threadGroupB));
        last->m_scene = this;
        last->m_index = 0x3f;
        M2SceneDrawListAnimate(last);
        ThreadPoolGroupWait(static_cast<THREADPOOLGROUP*>(this->m_threadGroupB));
        ThreadPoolGroupSetCount(static_cast<THREADPOOLGROUP*>(this->m_threadGroupB), 0x3f);
        for (uint32_t i = 0; i < 0x3f; i++) {
            M2SceneDrawList* list = &this->m_drawLists[i];
            list->m_scene = this;
            list->m_index = i;
            SetListTask(&tasks[i], M2SceneDrawListPostAnimate, list);
        }
        ThreadPoolSubmit(0x3f, tasks, 2, static_cast<THREADPOOLGROUP*>(this->m_threadGroupB));
        last->m_scene = this;
        last->m_index = 0x3f;
        M2SceneDrawListPostAnimate(last);
        ThreadPoolGroupWait(static_cast<THREADPOOLGROUP*>(this->m_threadGroupB));
        if (trackRects) {
            for (uint32_t i = 0; i < 0x40; i++) {
                MergeRect(this->m_screenRect, this->m_drawLists[i].m_screenRect, true);
            }
        }
        DestroyTasks(tasks);
    }
    int32_t total = 0;
    for (uint32_t i = 0; i < 0x40; i++) {
        total += this->m_drawLists[i].m_2f4;
    }
    this->m_db30 = total;
}

// ref: FUN_1411d77a0
void CM2Scene::FlushLodChanges() {
    auto models = reinterpret_cast<M2SceneVector<CM2BaseModel*>*>(&this->m_f60);
    for (uint32_t i = 0; i < models->m_size; i++) {
        models->m_data[i]->OnLodChanged();
    }
    if (models->m_size != 0) {
        models->m_size = 0;
    }
}
