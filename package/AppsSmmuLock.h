/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* From UniXNU-sheng package/AppsSmmuLock.h (BSD-2-Clause-Patent), unchanged but for this line. */
#ifndef SHENG_APPS_SMMU_LOCK_H
#define SHENG_APPS_SMMU_LOCK_H
extern "C" bool sm8550_apps_smmu_lock(void) __attribute__((weak_import));
extern "C" void sm8550_apps_smmu_unlock(void) __attribute__((weak_import));
class AppsSmmuLock {
    bool held;
public:
    AppsSmmuLock():held(sm8550_apps_smmu_lock && sm8550_apps_smmu_unlock && sm8550_apps_smmu_lock()) {}
    ~AppsSmmuLock(){if(held)sm8550_apps_smmu_unlock();}
    bool ready() const {return held || !sm8550_apps_smmu_lock;}
    AppsSmmuLock(const AppsSmmuLock&)=delete;
    AppsSmmuLock& operator=(const AppsSmmuLock&)=delete;
};
#endif
