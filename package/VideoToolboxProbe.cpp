/* SPDX-License-Identifier: GPL-2.0-only */
/* The board half of VideoToolbox's paravirtualized codecs on sheng: once VideoProbe.cpp has the iris platform up, the transport
 * registers its classes and publishes AppleVideoToolboxParavirtualizationDriver (drivers/QcomIris/kernel/VtParavirt.cpp), whose
 * host (VtHost.cpp) decodes on the iris decoder device (IrisMedia.cpp). vt-paravirt=0 leaves VideoToolbox its own codecs. */
extern "C" int vt_paravirt_attach(void);
/* 1 attached, -1 failed, 0 not reached */
extern "C" volatile int native_vtpv_stats[4];
volatile int native_vtpv_stats[4];

/* Called by VideoProbe.cpp once the iris platform is ready. */
extern "C" void qcom_iris_video_publish(void)
{
    native_vtpv_stats[0] = vt_paravirt_attach() ? -1 : 1;
}
