#include <gtest/gtest.h>
#include <cerrno>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "../switch/include/switch/bootstrap_rules.h"

namespace {
constexpr uint32_t POSIX_SPAWN_PERSONA_FLAGS_OVERRIDE = 1, POSIX_SPAWN_PERSONA_FLAGS_VERIFY = 2;
constexpr uint32_t POSIX_SPAWN_PERSONA_UID = 0x10000, POSIX_SPAWN_PERSONA_GID = 0x20000;
constexpr uint32_t POSIX_SPAWN_PERSONA_GROUPS = 0x40000;
constexpr int PERSONA_SYSTEM = 4;
constexpr const char *PERSONA_MGMT_ENTITLEMENT = "com.apple.private.persona-mgmt";
struct _posix_spawn_persona_info {
    uint32_t pspi_id, pspi_flags, pspi_uid, pspi_gid, pspi_ngroups = 0;
    uint32_t pspi_groups[NGROUPS_MAX]{}, pspi_gmuid = 0;
};
struct persona { uint32_t id, uid; int type; std::string login; int references = 1; };
struct credential { bool root = false; uint32_t uid = 501, gid = 501; } current_credential;
using kauth_cred_t = credential *;
using kauth_cred_derive_t = bool (^)(kauth_cred_t, kauth_cred_t);
constexpr uint32_t KAUTH_UID_NONE = UINT32_MAX - 100;
constexpr int PROC_SETTOKEN_SETUGID = 1;
bool entitled = true, admitted = true;
int allocation_error, initialization_error, adoption_error, allocations, initializations, finalizations, credential_updates;
int switch_bootstrap_persona_lock;
persona *registered_persona, *switch_bootstrap_persona;
std::vector<std::unique_ptr<persona>> personas;
struct proc { persona *p_persona = nullptr; credential credentials{}; };
using proc_t = proc *;
void lck_mtx_lock(int *) {}
void lck_mtx_unlock(int *) {}
int current_proc() { return 1; }
bool switch_bootstrap_persona_caller(int) { return admitted; }
kauth_cred_t kauth_cred_get() { return &current_credential; }
bool kauth_cred_issuser(kauth_cred_t cred) { return cred->root; }
bool IOCurrentTaskHasEntitlement(const char *name)
{
    return entitled && !std::strcmp(name, PERSONA_MGMT_ENTITLEMENT);
}
bool kauth_cred_model_setresuid(kauth_cred_t model, uint32_t real, uint32_t effective, uint32_t saved, uint32_t gmuid)
{
    EXPECT_EQ(real, effective);
    EXPECT_EQ(saved, effective);
    EXPECT_EQ(gmuid, KAUTH_UID_NONE);
    model->uid = effective;
    model->root = effective == 0;
    return true;
}
bool kauth_cred_model_setresgid(kauth_cred_t model, uint32_t real, uint32_t effective, uint32_t saved)
{
    EXPECT_EQ(real, effective);
    EXPECT_EQ(saved, effective);
    model->gid = effective;
    return true;
}
bool kauth_cred_model_setgroups(kauth_cred_t, const uint32_t *, uint32_t, uint32_t) { return true; }
bool kauth_cred_proc_update(proc_t p, int action, kauth_cred_derive_t derive)
{
    EXPECT_EQ(action, PROC_SETTOKEN_SETUGID);
    ++credential_updates;
    return derive(&current_credential, &p->credentials);
}
persona *persona_lookup(uint32_t id)
{
    if (!registered_persona || registered_persona->id != id) return nullptr;
    ++registered_persona->references;
    return registered_persona;
}
void persona_put(persona *value) { --value->references; }
int persona_proc_adopt(proc_t p, persona *value, kauth_cred_derive_t derive)
{
    if (!adoption_error && p->p_persona != value) {
        kauth_cred_proc_update(p, PROC_SETTOKEN_SETUGID, derive);
        p->p_persona = value;
    }
    persona_put(value);
    return adoption_error;
}
persona *persona_alloc(uint32_t id, const char *login, int type, char *path, uint32_t uid, int *error)
{
    ++allocations;
    *error = allocation_error;
    if (*error) return nullptr;
    EXPECT_EQ(path, nullptr);
    personas.push_back(std::make_unique<persona>(persona{id, uid, type, login}));
    return personas.back().get();
}
int persona_init_begin(persona *) { ++initializations; return initialization_error; }
void persona_init_end(persona *value, int error)
{
    ++finalizations;
    if (!error) registered_persona = value;
}
#include "bootstrap_persona.h"
#include "bootstrap_persona_validate_17.h"
#include "bootstrap_persona_validate_27.h"
#include "bootstrap_persona_adopt_17.h"
#include "bootstrap_persona_adopt_27.h"

class BootstrapPersona : public testing::Test {
protected:
    _posix_spawn_persona_info request{SWITCH_BOOTSTRAP_PERSONA,
        POSIX_SPAWN_PERSONA_FLAGS_OVERRIDE | POSIX_SPAWN_PERSONA_UID | POSIX_SPAWN_PERSONA_GID, 0, 0};
    void SetUp() override
    {
        current_credential = {};
        entitled = admitted = true;
        allocation_error = initialization_error = adoption_error = allocations = initializations = finalizations = 0;
        credential_updates = 0;
        registered_persona = switch_bootstrap_persona = nullptr;
        personas.clear();
    }
};

TEST_F(BootstrapPersona, AuthorizedSpawnCreatesAndRetainsNativeRootPersona)
{
    for (auto validate : {spawn_validate_persona_17, spawn_validate_persona_27}) {
        ASSERT_EQ(validate(&request), 0);
        ASSERT_NE(registered_persona, nullptr);
        EXPECT_EQ(registered_persona->id, SWITCH_BOOTSTRAP_PERSONA);
        EXPECT_EQ(registered_persona->uid, 0u);
        EXPECT_EQ(registered_persona->type, PERSONA_SYSTEM);
        EXPECT_EQ(registered_persona->login, "SwitchBootstrap");
        EXPECT_EQ(registered_persona->references, 1);
    }
    EXPECT_EQ(allocations, 1);
    EXPECT_EQ(initializations, 1);
    EXPECT_EQ(finalizations, 1);
}

TEST_F(BootstrapPersona, OrdinaryPersonaEntitlementCannotRequestRoot)
{
    ASSERT_EQ(switch_bootstrap_persona_init(), 0);
    admitted = false;
    for (auto validate : {spawn_validate_persona_17, spawn_validate_persona_27}) {
        EXPECT_EQ(validate(&request), EPERM);
        EXPECT_EQ(registered_persona->references, 1);
    }
    admitted = true;
    entitled = false;
    for (auto validate : {spawn_validate_persona_17, spawn_validate_persona_27})
        EXPECT_EQ(validate(&request), EPERM);
}

TEST_F(BootstrapPersona, MissingPersonaNeverFallsBackToRootCredentials)
{
    admitted = false;
    for (auto validate : {spawn_validate_persona_17, spawn_validate_persona_27})
        EXPECT_EQ(validate(&request), ESRCH);
    EXPECT_EQ(allocations, 0);
    EXPECT_EQ(registered_persona, nullptr);
}

TEST_F(BootstrapPersona, RootExceptionRequiresExactFlagsAndPersona)
{
    ASSERT_EQ(switch_bootstrap_persona_init(), 0);
    for (uint32_t flags : {POSIX_SPAWN_PERSONA_FLAGS_OVERRIDE, POSIX_SPAWN_PERSONA_UID,
                           POSIX_SPAWN_PERSONA_GID, request.pspi_flags | POSIX_SPAWN_PERSONA_FLAGS_VERIFY,
                           request.pspi_flags | POSIX_SPAWN_PERSONA_GROUPS, request.pspi_flags | 0x80000000u}) {
        auto changed = request;
        changed.pspi_flags = flags;
        EXPECT_FALSE(switch_bootstrap_persona_request(&changed));
        for (auto validate : {spawn_validate_persona_17, spawn_validate_persona_27})
            EXPECT_EQ(validate(&changed), EPERM);
    }
    request.pspi_id = 501;
    EXPECT_FALSE(switch_bootstrap_persona_request(&request));
    request.pspi_id = SWITCH_BOOTSTRAP_PERSONA;
    request.pspi_uid = 501;
    EXPECT_FALSE(switch_bootstrap_persona_request(&request));
    request.pspi_uid = 0;
    request.pspi_gid = 501;
    EXPECT_FALSE(switch_bootstrap_persona_request(&request));
    EXPECT_FALSE(switch_bootstrap_persona_request(nullptr));
}

TEST_F(BootstrapPersona, AllocationAndInitializationFailuresReturnWithoutRootFallback)
{
    allocation_error = EBUSY;
    for (auto validate : {spawn_validate_persona_17, spawn_validate_persona_27})
        EXPECT_EQ(validate(&request), EBUSY);
    EXPECT_EQ(registered_persona, nullptr);
    allocation_error = 0;
    initialization_error = EEXIST;
    for (auto validate : {spawn_validate_persona_17, spawn_validate_persona_27})
        EXPECT_EQ(validate(&request), EEXIST);
    EXPECT_EQ(initializations, finalizations);
    for (const auto &value : personas) EXPECT_EQ(value->references, 0);
    EXPECT_EQ(switch_bootstrap_persona, nullptr);
}

TEST_F(BootstrapPersona, ExistingNativePersonaIsRetainedWithoutChangingItsMetadata)
{
    persona native{SWITCH_BOOTSTRAP_PERSONA, 501, 1, "ApplePersona"};
    registered_persona = &native;
    for (auto validate : {spawn_validate_persona_17, spawn_validate_persona_27})
        EXPECT_EQ(validate(&request), 0);
    EXPECT_EQ(switch_bootstrap_persona, &native);
    EXPECT_EQ(native.references, 2);
    EXPECT_EQ(native.id, SWITCH_BOOTSTRAP_PERSONA);
    EXPECT_EQ(native.uid, 501u);
    EXPECT_EQ(native.type, 1);
    EXPECT_EQ(native.login, "ApplePersona");
    EXPECT_EQ(allocations, 0);
}

TEST_F(BootstrapPersona, RetainedPersonaCannotBeReplacedOrRecreatedAfterInvalidation)
{
    persona native{SWITCH_BOOTSTRAP_PERSONA, 0, PERSONA_SYSTEM, "ApplePersona"};
    persona replacement{SWITCH_BOOTSTRAP_PERSONA, 0, PERSONA_SYSTEM, "Replacement"};
    registered_persona = &native;
    ASSERT_EQ(switch_bootstrap_persona_init(), 0);
    registered_persona = &replacement;
    for (auto validate : {spawn_validate_persona_17, spawn_validate_persona_27})
        EXPECT_EQ(validate(&request), EEXIST);
    EXPECT_EQ(native.references, 2);
    EXPECT_EQ(replacement.references, 1);
    registered_persona = nullptr;
    for (auto validate : {spawn_validate_persona_17, spawn_validate_persona_27})
        EXPECT_EQ(validate(&request), ESRCH);
    EXPECT_EQ(switch_bootstrap_persona, &native);
    EXPECT_EQ(allocations, 0);
}

TEST_F(BootstrapPersona, SamePersonaAppliesRootCredentialsOnlyForTheAuthorizedRequest)
{
    persona native{SWITCH_BOOTSTRAP_PERSONA, 501, 1, "ApplePersona"};
    registered_persona = &native;
    ASSERT_EQ(switch_bootstrap_persona_init(), 0);
    for (auto adopt : {spawn_persona_adopt_17, spawn_persona_adopt_27}) {
        for (bool authorized : {false, true}) {
            admitted = authorized;
            proc child{&native};
            credential_updates = 0;
            EXPECT_EQ(adopt(&child, &request), 0);
            EXPECT_EQ(child.credentials.uid, authorized ? 0u : 501u);
            EXPECT_EQ(child.credentials.gid, authorized ? 0u : 501u);
            EXPECT_EQ(credential_updates, authorized ? 1 : 0);
            EXPECT_EQ(native.references, 2);
        }
        admitted = true;
        auto changed = request;
        changed.pspi_flags |= POSIX_SPAWN_PERSONA_FLAGS_VERIFY;
        proc child{&native};
        credential_updates = 0;
        EXPECT_EQ(adopt(&child, &changed), 0);
        EXPECT_EQ(child.credentials.uid, 501u);
        EXPECT_EQ(child.credentials.gid, 501u);
        EXPECT_EQ(credential_updates, 0);
    }
}

TEST_F(BootstrapPersona, FailedNativeAdoptionNeverChangesSamePersonaCredentials)
{
    persona native{SWITCH_BOOTSTRAP_PERSONA, 0, PERSONA_SYSTEM, "ApplePersona"};
    registered_persona = &native;
    ASSERT_EQ(switch_bootstrap_persona_init(), 0);
    adoption_error = EINVAL;
    for (auto adopt : {spawn_persona_adopt_17, spawn_persona_adopt_27}) {
        proc child{&native};
        EXPECT_EQ(adopt(&child, &request), EINVAL);
        EXPECT_EQ(child.credentials.uid, 501u);
        EXPECT_EQ(child.credentials.gid, 501u);
        EXPECT_EQ(native.references, 2);
    }
    EXPECT_EQ(credential_updates, 0);
}

TEST_F(BootstrapPersona, DifferentPersonaKeepsTheNativeCredentialDerivation)
{
    ASSERT_EQ(switch_bootstrap_persona_init(), 0);
    for (auto adopt : {spawn_persona_adopt_17, spawn_persona_adopt_27}) {
        proc child{};
        credential_updates = 0;
        EXPECT_EQ(adopt(&child, &request), 0);
        EXPECT_EQ(child.p_persona, switch_bootstrap_persona);
        EXPECT_EQ(child.credentials.uid, 0u);
        EXPECT_EQ(child.credentials.gid, 0u);
        EXPECT_EQ(credential_updates, 1);
    }
}

TEST_F(BootstrapPersona, NativeRootAndNonRootPersonaRulesArePreserved)
{
    persona foreign{501, 501, PERSONA_SYSTEM, "ApplePersona"};
    registered_persona = &foreign;
    request.pspi_id = 501;
    admitted = false;
    current_credential.root = true;
    for (auto validate : {spawn_validate_persona_17, spawn_validate_persona_27})
        EXPECT_EQ(validate(&request), 0);
    current_credential.root = false;
    request.pspi_uid = request.pspi_gid = 501;
    for (auto validate : {spawn_validate_persona_17, spawn_validate_persona_27})
        EXPECT_EQ(validate(&request), 0);
    request.pspi_flags |= POSIX_SPAWN_PERSONA_GROUPS;
    request.pspi_ngroups = NGROUPS_MAX + 1;
    for (auto validate : {spawn_validate_persona_17, spawn_validate_persona_27})
        EXPECT_EQ(validate(&request), EINVAL);
    EXPECT_EQ(allocations, 0);
    EXPECT_EQ(foreign.references, 1);
}


}
