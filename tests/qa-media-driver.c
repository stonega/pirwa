/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <glib.h>
#include <libvirt/libvirt.h>

int __real_virDomainUpdateDeviceFlags (virDomainPtr domain, const char *xml, unsigned int flags);

int
__wrap_virDomainUpdateDeviceFlags (virDomainPtr domain, const char *xml, unsigned int flags)
{
  /* A guest can keep its optical tray locked. Require force for a live eject,
   * then forward the mutation to the real test driver, which lacks that flag. */
  if (virDomainIsActive (domain) == 1)
    {
      g_assert_true ((flags & VIR_DOMAIN_DEVICE_MODIFY_FORCE) != 0);
      g_assert_true ((flags & VIR_DOMAIN_AFFECT_LIVE) != 0);
    }
  else
    g_assert_false ((flags & VIR_DOMAIN_DEVICE_MODIFY_FORCE) != 0);
  g_assert_cmpint ((flags & VIR_DOMAIN_AFFECT_CONFIG) != 0, ==,
                   virDomainIsPersistent (domain) == 1);
  return __real_virDomainUpdateDeviceFlags (domain, xml, flags & ~VIR_DOMAIN_DEVICE_MODIFY_FORCE);
}
