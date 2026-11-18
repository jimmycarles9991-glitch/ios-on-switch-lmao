#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstdint>
#include "../xnu-12377.121.6/osfmk/kern/cs_blobs.h"
#include "../switch/include/switch/bootstrap_rules.h"

namespace {
struct cs_blob {
    const CS_CodeDirectory *csb_cd = nullptr;
    const void *csb_hashtype = nullptr;
    void *csb_entitlements = nullptr;
    unsigned csb_flags = CS_VALID, csb_validation_category = 0, csb_reconstituted = 0;
    cs_blob *csb_next = nullptr;
    bool platform = true;
};
struct vnode {
    bool admitted;
    cs_blob *blob = nullptr;
    int64_t offset = 0;
};
struct proc {
    vnode *p_textvp;
    int64_t p_textoff;
    unsigned int flags;
};
constexpr int VM_PROT_READ = 1, VM_PROT_WRITE = 2, VM_PROT_EXECUTE = 4;
unsigned proc_getcsflags(proc *p) { return p->flags; }
bool switch_bootstrap_image(vnode *vp) { return vp->admitted; }
cs_blob *csvnode_get_blob(vnode *vp, int64_t offset) { return offset == vp->offset ? vp->blob : nullptr; }
unsigned csblob_get_flags(cs_blob *blob) { return blob->csb_flags; }
bool csblob_get_platform_binary(cs_blob *blob) { return blob->platform; }
cs_blob *ubc_get_cs_blobs(vnode *vp) { return vp->blob; }
int xml_error, der_error;
bool has_xml, has_der, valid_der;
std::array<uint64_t, 4> encoded{};
int csblob_get_entitlements(cs_blob *, void **xml, size_t *size)
{
    *xml = has_xml ? encoded.data() : nullptr;
    *size = has_xml ? encoded.size() * sizeof(encoded[0]) : 0;
    return xml_error;
}
int csblob_get_der_entitlements(cs_blob *, const CS_GenericBlob **der, size_t *size)
{
    *der = has_der ? reinterpret_cast<const CS_GenericBlob *>(encoded.data()) : nullptr;
    *size = has_der ? encoded.size() * sizeof(encoded[0]) : 0;
    return der_error;
}
const char *csblob_get_identity(cs_blob *) { return "test.identity"; }
void *switch_app_code_native_entitlements(const void *, size_t, const char *, cs_blob *)
{
    return valid_der ? encoded.data() : nullptr;
}
#include "bootstrap_signature.h"

class BootstrapSignature : public testing::Test {
protected:
    CS_CodeDirectory directory{};
    cs_blob blob;
    vnode file{true, &blob};
    unsigned flags = CS_VALID;
    bool admitted = false;
    void SetUp() override
    {
        directory.nCodeSlots = htonl(1);
        blob.csb_cd = &directory;
        blob.csb_hashtype = &directory;
        xml_error = der_error = 0;
        has_xml = has_der = false;
        valid_der = true;
    }
};

TEST_F(BootstrapSignature, UnprivilegedFilesCannotAcquireBootstrapSigningPrivileges)
{
    file.admitted = false;
    EXPECT_EQ(switch_bootstrap_signature(&file, &blob, &flags, &admitted), 0);
    EXPECT_FALSE(admitted);
    EXPECT_EQ(flags, CS_VALID);
    EXPECT_EQ(blob.csb_reconstituted, 0u);
    blob.csb_reconstituted = SWITCH_BOOTSTRAP_SIGNATURE;
    EXPECT_EQ(switch_bootstrap_signature(&file, &blob, &flags, &admitted), EPERM);
    EXPECT_EQ(switch_bootstrap_validate(&file), EPERM);
}

TEST_F(BootstrapSignature, NativeHashAndDerFailuresDenyPrivilegedAdmission)
{
    xml_error = EBADEXEC;
    EXPECT_EQ(switch_bootstrap_signature(&file, &blob, &flags, &admitted), EBADEXEC);
    xml_error = 0;
    has_xml = true;
    EXPECT_EQ(switch_bootstrap_signature(&file, &blob, &flags, &admitted), EBADEXEC);
    has_der = true;
    der_error = EBADEXEC;
    EXPECT_EQ(switch_bootstrap_signature(&file, &blob, &flags, &admitted), EBADEXEC);
    der_error = 0;
    valid_der = false;
    EXPECT_EQ(switch_bootstrap_signature(&file, &blob, &flags, &admitted), EBADEXEC);
    EXPECT_EQ(flags, CS_VALID);
    directory.nCodeSlots = 0;
    EXPECT_EQ(switch_bootstrap_signature(&file, &blob, &flags, &admitted), EBADEXEC);
}

TEST_F(BootstrapSignature, PreservesEntitlementsAndRecordsRevocableProvenance)
{
    has_xml = has_der = true;
    ASSERT_EQ(switch_bootstrap_signature(&file, &blob, &flags, &admitted), 0);
    EXPECT_EQ(blob.csb_entitlements, encoded.data());
    EXPECT_TRUE(flags & CS_ENTITLEMENTS_VALIDATED);
    EXPECT_TRUE(flags & CS_PLATFORM_BINARY);
    EXPECT_TRUE(admitted);
    EXPECT_EQ(blob.csb_reconstituted, 0u);
    blob.csb_reconstituted = SWITCH_BOOTSTRAP_SIGNATURE;
    EXPECT_EQ(switch_bootstrap_validate(&file), 0);
    file.admitted = false;
    EXPECT_EQ(switch_bootstrap_validate(&file), EPERM);
}

TEST_F(BootstrapSignature, PreservesNativeReconstitutionUntilPublication)
{
    blob.csb_reconstituted = true;
    ASSERT_EQ(switch_bootstrap_signature(&file, &blob, &flags, &admitted), 0);
    EXPECT_TRUE(admitted);
    EXPECT_EQ(blob.csb_entitlements, nullptr);
    EXPECT_EQ(blob.csb_reconstituted, 1u);
}

TEST_F(BootstrapSignature, FilePageTrustRequiresBootstrapProvenance)
{
    EXPECT_FALSE(switch_bootstrap_signed(nullptr));
    for (unsigned provenance : {0u, 1u, 2u}) {
        blob.csb_reconstituted = provenance;
        EXPECT_FALSE(switch_bootstrap_signed(&file));
    }
    cs_blob admitted;
    admitted.csb_reconstituted = SWITCH_BOOTSTRAP_SIGNATURE;
    blob.csb_next = &admitted;
    EXPECT_TRUE(switch_bootstrap_signed(&file));
}

TEST_F(BootstrapSignature, SandboxPreservationRequiresCurrentMembershipAndTheExactNativeSignature)
{
    blob.csb_flags = CS_VALID | CS_SIGNED | CS_ENTITLEMENTS_VALIDATED;
    blob.csb_reconstituted = SWITCH_BOOTSTRAP_SIGNATURE;
    ASSERT_TRUE(switch_bootstrap_sandbox_target(&file, 0));
    for (unsigned flag : {CS_VALID, CS_SIGNED, CS_ENTITLEMENTS_VALIDATED}) {
        blob.csb_flags &= ~flag;
        EXPECT_FALSE(switch_bootstrap_sandbox_target(&file, 0));
        blob.csb_flags |= flag;
    }
    for (unsigned provenance : {0u, 1u, 2u}) {
        blob.csb_reconstituted = provenance;
        EXPECT_FALSE(switch_bootstrap_sandbox_target(&file, 0));
    }
    blob.csb_reconstituted = SWITCH_BOOTSTRAP_SIGNATURE;
    file.admitted = false;
    EXPECT_FALSE(switch_bootstrap_sandbox_target(&file, 0));
    file.admitted = true;
    blob.platform = false;
    EXPECT_FALSE(switch_bootstrap_sandbox_target(&file, 0));
    blob.platform = true;
    EXPECT_FALSE(switch_bootstrap_sandbox_target(&file, 1));
    EXPECT_FALSE(switch_bootstrap_sandbox_target(nullptr, 0));
}

TEST_F(BootstrapSignature, ExecutableMappingsRequireBothExactSignatureOffsetsAndReadOnlyExecution)
{
    blob.csb_flags = CS_VALID | CS_SIGNED | CS_ENTITLEMENTS_VALIDATED;
    blob.csb_reconstituted = SWITCH_BOOTSTRAP_SIGNATURE;
    file.offset = 0x4000;
    cs_blob library_blob = blob;
    vnode library{true, &library_blob, 0x8000};
    proc caller{&file, file.offset, CS_VALID | CS_SIGNED};
    constexpr int rx = VM_PROT_READ | VM_PROT_EXECUTE;
    ASSERT_TRUE(switch_bootstrap_exec_mapping(&caller, &library, 0x8000, rx));
    EXPECT_FALSE(switch_bootstrap_exec_mapping(&caller, &library, 0x4000, rx));
    caller.p_textoff = 0;
    EXPECT_FALSE(switch_bootstrap_exec_mapping(&caller, &library, 0x8000, rx));
    caller.p_textoff = file.offset;
    EXPECT_FALSE(switch_bootstrap_exec_mapping(&caller, &library, 0x8000, VM_PROT_READ));
    EXPECT_FALSE(switch_bootstrap_exec_mapping(&caller, &library, 0x8000, rx | VM_PROT_WRITE));
    library.offset = INT64_MIN;
    EXPECT_FALSE(switch_bootstrap_exec_mapping(&caller, &library, uint64_t{INT64_MAX} + 1, rx));
}

TEST_F(BootstrapSignature, ExecutableMappingRejectsImportedRevokedAndStaleCode)
{
    blob.csb_flags = CS_VALID | CS_SIGNED | CS_ENTITLEMENTS_VALIDATED;
    blob.csb_reconstituted = SWITCH_BOOTSTRAP_SIGNATURE;
    cs_blob library_blob = blob;
    vnode library{true, &library_blob};
    proc caller{&file, 0, CS_VALID | CS_SIGNED};
    ASSERT_TRUE(switch_bootstrap_exec_mapping(&caller, &library, 0, VM_PROT_EXECUTE));
    for (cs_blob *signature : {&blob, &library_blob}) {
        signature->csb_reconstituted = 2;
        EXPECT_FALSE(switch_bootstrap_exec_mapping(&caller, &library, 0, VM_PROT_EXECUTE));
        signature->csb_reconstituted = SWITCH_BOOTSTRAP_SIGNATURE;
    }
    for (vnode *image : {&file, &library}) {
        image->admitted = false;
        EXPECT_FALSE(switch_bootstrap_exec_mapping(&caller, &library, 0, VM_PROT_EXECUTE));
        image->admitted = true;
    }
    for (unsigned flag : {CS_VALID, CS_SIGNED}) {
        caller.flags &= ~flag;
        EXPECT_FALSE(switch_bootstrap_exec_mapping(&caller, &library, 0, VM_PROT_EXECUTE));
        caller.flags |= flag;
    }
}
}
