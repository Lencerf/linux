#include <kunit/test.h>

#include <linux/slab.h>
#include <linux/libfdt.h>
#include <linux/kexec.h>


static void kho_add_remove_nodes(struct kunit *test) {
    struct kho_node n1 = KHO_NODE_INIT;
    struct kho_node n2 = KHO_NODE_INIT;

    KUNIT_EXPECT_EQ(test, kho_add_node(&n1, "n2", &n2), 0);
    KUNIT_EXPECT_EQ(test, kho_add_node(&n1, "n2", &n2), -EEXIST);

    KUNIT_EXPECT_PTR_EQ(test, kho_remove_node(&n1, "unknown"), ERR_PTR(-ENOENT));
    KUNIT_EXPECT_PTR_EQ(test, kho_remove_node(&n1, "n2"), &n2);
    KUNIT_EXPECT_PTR_EQ(test, kho_remove_node(&n1, "n2"), ERR_PTR(-ENOENT));
}

static void kho_add_remove_properties(struct kunit *test) {
    struct kho_node n1 = KHO_NODE_INIT;
    unsigned long v1 = 870621;
    const char* s = "string_val";
    u32 v1_size = 0;

    KUNIT_EXPECT_EQ(test, kho_add_prop(&n1, "v1", &v1, sizeof(v1)), 0);
    KUNIT_EXPECT_EQ(test, kho_add_prop(&n1, "v1", &v1, sizeof(v1)), -EEXIST);
    KUNIT_EXPECT_PTR_EQ(test, kho_remove_prop(&n1, "v1", &v1_size), &v1);
    KUNIT_EXPECT_EQ(test, v1_size, sizeof(v1));
    KUNIT_EXPECT_PTR_EQ(test, kho_remove_prop(&n1, "v1", NULL), ERR_PTR(-ENOENT));

    KUNIT_EXPECT_EQ(test, kho_add_string_prop(&n1, "s", s), 0);
    KUNIT_EXPECT_PTR_EQ(test, kho_remove_prop(&n1, "s", NULL), s);
}

static struct kunit_case kho_cases[] = {
    KUNIT_CASE(kho_add_remove_nodes),
    KUNIT_CASE(kho_add_remove_properties),
    {}
};

static struct kunit_suite kho_test_suite = {
    .name = "kho_test",
    .test_cases = kho_cases,
};
kunit_test_suite(kho_test_suite);


MODULE_LICENSE("GPL");