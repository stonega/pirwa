/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pirwa-config.h"
#include "pirwa-network.h"

typedef struct
{
  const char *type;
  const char *model;
  const char *source;
} Network;

static void
test_runtime (gconstpointer data)
{
  const Network *network = data;
  g_autofree char *original = g_strdup_printf (
      "<domain type='test'><name>proxy-test</name>"
      "<uuid>88888888-8888-4888-8888-888888888888</uuid><memory>131072</memory>"
      "<vcpu>1</vcpu><os><type>hvm</type></os><devices><interface type='%s'>"
      "<mac address='52:54:00:12:34:56'/><model type='%s'/>%s"
      "<address type='pci' domain='0x0000' bus='0x00' slot='0x03' function='0x0'/>"
      "<link state='up'/></interface></devices></domain>",
      network->type, network->model, network->source);
  g_autoptr (GError) error = NULL;
  g_autofree char *xml = pirwa_network_domain (original, 34567, &error);
  g_assert_no_error (error);
  g_assert_true (pirwa_config_supports (
      xml,
      "/domain/devices/interface[@type='client']/source[@address='127.0.0.1'][@port='34567']"));
  g_assert_true (
      pirwa_config_supports (xml, "/domain/devices/interface/mac[@address='52:54:00:12:34:56']"));
  g_autofree char *model
      = g_strdup_printf ("/domain/devices/interface/model[@type='%s']", network->model);
  g_assert_true (pirwa_config_supports (xml, model));
  g_assert_true (
      pirwa_config_supports (xml, "/domain/devices/interface/address[@type='pci'][@slot='0x03']"));
  g_assert_true (pirwa_config_supports (xml, "/domain/devices/interface/link[@state='up']"));
  g_assert_false (pirwa_config_supports (xml, "/domain/devices/interface/backend"));
  virConnectPtr connection = virConnectOpen ("test:///default");
  g_assert_nonnull (connection);
  virDomainPtr domain = virDomainDefineXML (connection, original);
  g_assert_nonnull (domain);
  g_autofree char *before = virDomainGetXMLDesc (domain, VIR_DOMAIN_XML_INACTIVE);
  virDomainPtr running = virDomainCreateXML (connection, xml, 0);
  g_assert_nonnull (running);
  g_assert_cmpint (virDomainIsPersistent (running), ==, 1);
  g_autofree char *live = virDomainGetXMLDesc (running, 0);
  g_assert_true (pirwa_config_supports (live, "/domain/devices/interface[@type='client']"));
  g_autofree char *saved = virDomainGetXMLDesc (running, VIR_DOMAIN_XML_INACTIVE);
  g_assert_cmpstr (saved, ==, before);
  g_assert_cmpint (virDomainDestroy (running), ==, 0);
  g_autofree char *after = virDomainGetXMLDesc (domain, VIR_DOMAIN_XML_INACTIVE);
  g_assert_cmpstr (after, ==, before);
  g_assert_cmpint (virDomainUndefine (domain), ==, 0);
  virDomainFree (running);
  virDomainFree (domain);
  virConnectClose (connection);
}

static void
test_reject (void)
{
  const char *unsupported[]
      = { "<domain><devices><interface type='bridge'/></devices></domain>",
          "<domain><devices><interface type='network'><source network='default' "
          "portgroup='custom'/></interface></devices></domain>",
          "<domain><devices><interface type='bridge'><source bridge='virbr0'>"
          "<custom/></source></interface></devices></domain>",
          "<domain><devices><interface type='bridge'><source bridge='virbr0'/>"
          "<source bridge='virbr1'/></interface></devices></domain>",
          "<domain><devices><interface type='bridge'><source bridge='virbr0'/>"
          "<filterref filter='clean-traffic'/></interface></devices></domain>",
          "<domain><devices><interface type='bridge'><source bridge='virbr0'/>"
          "<virtualport type='openvswitch'/></interface></devices></domain>",
          "<domain><devices><interface type='bridge'><source bridge='virbr0'/>"
          "<vlan><tag id='42'/></vlan></interface></devices></domain>",
          "<domain><devices><interface type='network'><source network='default'/>"
          "<bandwidth/></interface></devices></domain>",
          "<domain><devices><interface type='user'><ip address='192.0.2.2'/></interface>"
          "</devices></domain>",
          "<domain><devices><interface type='user'><source address='192.0.2.0'/>"
          "</interface></devices></domain>",
          "<domain><devices><interface type='user'><route address='192.0.2.0'/>"
          "</interface></devices></domain>",
          "<domain><devices><interface type='user'/><interface type='user'/></devices></domain>",
          "<domain><devices><interface type='user'><portForward "
          "proto='tcp'/></interface></devices></domain>",
          "<domain><devices/></domain>",
          "<!DOCTYPE domain><domain/>",
          "not xml" };
  for (guint i = 0; i < G_N_ELEMENTS (unsupported); i++)
    {
      g_autoptr (GError) error = NULL;
      g_assert_null (pirwa_network_domain (unsupported[i], 34567, &error));
      g_assert_nonnull (error);
    }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  const Network networks[] = { { "user", "virtio", "<backend type='passt'/>" },
                               { "bridge", "rtl8139", "<source bridge='virbr0'/>" },
                               { "network", "virtio", "<source network='default'/>" } };
  for (guint i = 0; i < G_N_ELEMENTS (networks); i++)
    {
      g_autofree char *path = g_strdup_printf ("/network/runtime-only/%s", networks[i].type);
      g_test_add_data_func (path, &networks[i], test_runtime);
    }
  g_test_add_func ("/network/unsupported", test_reject);
  return g_test_run ();
}
