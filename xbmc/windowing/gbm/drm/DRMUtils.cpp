/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DRMUtils.h"

#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/StringUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"

using namespace KODI::WINDOWING::GBM;

namespace
{
const std::string SETTING_VIDEOSCREEN_LIMITGUISIZE = "videoscreen.limitguisize";

void DrmFbDestroyCallback(gbm_bo* bo, void* data)
{
  drm_fb* fb = static_cast<drm_fb*>(data);

  if (fb->fb_id > 0)
  {
    CLog::Log(LOGDEBUG, "CDRMUtils::{} - removing framebuffer: {}", __FUNCTION__, fb->fb_id);
    int drm_fd = gbm_device_get_fd(gbm_bo_get_device(bo));
    drmModeRmFB(drm_fd, fb->fb_id);
  }

  delete fb;
}
}

CDRMUtils::~CDRMUtils()
{
  DestroyDrm();
}

bool CDRMUtils::SetMode(const RESOLUTION_INFO& res)
{
  if (!m_connector->CheckConnector())
    return false;

  m_mode = m_connector->GetModeForIndex(std::atoi(res.strId.c_str()));
  m_width = res.iWidth;
  m_height = res.iHeight;

  CLog::Log(LOGDEBUG, "CDRMUtils::{} - found crtc mode: {}x{}{} @ {} Hz", __FUNCTION__,
            m_mode->hdisplay, m_mode->vdisplay, m_mode->flags & DRM_MODE_FLAG_INTERLACE ? "i" : "",
            m_mode->vrefresh);

  return true;
}

drm_fb * CDRMUtils::DrmFbGetFromBo(struct gbm_bo *bo)
{
  {
    struct drm_fb *fb = static_cast<drm_fb *>(gbm_bo_get_user_data(bo));
    if(fb)
    {
      if (m_gui_plane->GetFormat() == fb->format)
        return fb;
      else
        DrmFbDestroyCallback(bo, gbm_bo_get_user_data(bo));
    }
  }

  struct drm_fb *fb = new drm_fb;
  fb->bo = bo;
  fb->format = m_gui_plane->GetFormat();

  uint32_t width,
           height,
           handles[4] = {0},
           strides[4] = {0},
           offsets[4] = {0};

  uint64_t modifiers[4] = {0};

  width = gbm_bo_get_width(bo);
  height = gbm_bo_get_height(bo);

#if defined(HAS_GBM_MODIFIERS)
  for (int i = 0; i < gbm_bo_get_plane_count(bo); i++)
  {
    handles[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
    strides[i] = gbm_bo_get_stride_for_plane(bo, i);
    offsets[i] = gbm_bo_get_offset(bo, i);
    modifiers[i] = gbm_bo_get_modifier(bo);
  }
#else
  handles[0] = gbm_bo_get_handle(bo).u32;
  strides[0] = gbm_bo_get_stride(bo);
  memset(offsets, 0, 16);
#endif

  uint32_t flags = 0;

  if (modifiers[0] && modifiers[0] != DRM_FORMAT_MOD_INVALID)
  {
    flags |= DRM_MODE_FB_MODIFIERS;
    CLog::Log(LOGDEBUG, "CDRMUtils::{} - using modifier: {:#x}", __FUNCTION__, modifiers[0]);
  }

  int ret = drmModeAddFB2WithModifiers(m_fd,
                                       width,
                                       height,
                                       fb->format,
                                       handles,
                                       strides,
                                       offsets,
                                       modifiers,
                                       &fb->fb_id,
                                       flags);

  if(ret < 0)
  {
    ret = drmModeAddFB2(m_fd,
                        width,
                        height,
                        fb->format,
                        handles,
                        strides,
                        offsets,
                        &fb->fb_id,
                        flags);

    if (ret < 0)
    {
      delete (fb);
      CLog::Log(LOGDEBUG, "CDRMUtils::{} - failed to add framebuffer: {} ({})", __FUNCTION__, strerror(errno), errno);
      return nullptr;
    }
  }

  gbm_bo_set_user_data(bo, fb, DrmFbDestroyCallback);

  return fb;
}

bool CDRMUtils::FindPreferredMode()
{
  for (int i = 0, area = 0; i < m_connector->GetModesCount(); i++)
  {
    drmModeModeInfo* current_mode = m_connector->GetModeForIndex(i);

    if(current_mode->type & DRM_MODE_TYPE_PREFERRED)
    {
      m_mode = current_mode;
      CLog::Log(LOGDEBUG, "CDRMUtils::{} - found preferred mode: {}x{}{} @ {} Hz", __FUNCTION__,
                m_mode->hdisplay, m_mode->vdisplay,
                m_mode->flags & DRM_MODE_FLAG_INTERLACE ? "i" : "", m_mode->vrefresh);
      break;
    }

    auto current_area = current_mode->hdisplay * current_mode->vdisplay;
    if (current_area > area)
    {
      m_mode = current_mode;
      area = current_area;
    }
  }

  if(!m_mode)
  {
    CLog::Log(LOGDEBUG, "CDRMUtils::{} - failed to find preferred mode", __FUNCTION__);
    return false;
  }

  return true;
}

bool CDRMUtils::FindPlanes()
{
  for (size_t i = 0; i < m_crtcs.size(); i++)
  {
    const auto crtc = m_crtcs[i].get();
    if (!crtc)
      continue;

    auto videoPlane = std::find_if(m_planes.begin(), m_planes.end(), [this, &i](auto& plane) {
      if (plane->GetPossibleCrtcs() & (1 << i))
      {
        return plane->SupportsFormat(DRM_FORMAT_NV12);
      }
      return false;
    });

    uint32_t videoPlaneId{0};

    if (videoPlane != m_planes.end())
      videoPlaneId = videoPlane->get()->GetPlaneId();

    auto guiPlane =
        std::find_if(m_planes.begin(), m_planes.end(), [this, &i, &videoPlaneId](auto& plane) {
          if (plane->GetPossibleCrtcs() & (1 << i))
          {
            return (plane->GetPlaneId() != videoPlaneId &&
                    (videoPlaneId == 0 || plane->SupportsFormat(DRM_FORMAT_ARGB8888)) &&
                    plane->SupportsFormat(DRM_FORMAT_XRGB8888));
          }
          return false;
        });

    if (videoPlane->get() && guiPlane->get())
    {
      m_crtc = crtc;
      m_video_plane = videoPlane->get();
      m_gui_plane = guiPlane->get();
      break;
    }

    if (guiPlane->get())
    {
      if (!m_crtc && m_encoder->GetCrtcId() == crtc->GetCrtcId())
      {
        m_crtc = crtc;
        m_gui_plane = guiPlane->get();
      }
    }
  }

  // video plane may not be available
  if (m_video_plane)
    CLog::Log(LOGDEBUG, "CDRMUtils::{} - using video plane {}", __FUNCTION__,
              m_video_plane->GetPlaneId());

  CLog::Log(LOGDEBUG, "CDRMUtils::{} - using gui plane {}", __FUNCTION__,
            m_gui_plane->GetPlaneId());

  return true;
}

void CDRMUtils::PrintDrmDeviceInfo(drmDevicePtr device)
{
  CLog::Log(LOGDEBUG, "CDRMUtils::{} - Device Info:", __FUNCTION__);
  CLog::Log(LOGDEBUG, "CDRMUtils::{} -   available_nodes: {:#04x}", __FUNCTION__,
            device->available_nodes);
  CLog::Log(LOGDEBUG, "CDRMUtils::{} -   nodes:", __FUNCTION__);
  for (int i = 0; i < DRM_NODE_MAX; i++)
  {
    if (device->available_nodes & 1 << i)
      CLog::Log(LOGDEBUG, "CDRMUtils::{} -     nodes[{}]: {}", __FUNCTION__, i, device->nodes[i]);
  }

  CLog::Log(LOGDEBUG, "CDRMUtils::{} -   bustype: {:#04x}", __FUNCTION__, device->bustype);

  if (device->bustype == DRM_BUS_PCI)
  {
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -     pci:", __FUNCTION__);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -       domain: {:#04x}", __FUNCTION__,
              device->businfo.pci->domain);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -       bus:    {:#02x}", __FUNCTION__,
              device->businfo.pci->bus);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -       dev:    {:#02x}", __FUNCTION__,
              device->businfo.pci->dev);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -       func:   {:#1}", __FUNCTION__,
              device->businfo.pci->func);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -   deviceinfo:", __FUNCTION__);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -     pci:", __FUNCTION__);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -       vendor_id:    {:#04x}", __FUNCTION__,
              device->deviceinfo.pci->vendor_id);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -       device_id:    {:#04x}", __FUNCTION__,
              device->deviceinfo.pci->device_id);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -       subvendor_id: {:#04x}", __FUNCTION__,
              device->deviceinfo.pci->subvendor_id);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -       subdevice_id: {:#04x}", __FUNCTION__,
              device->deviceinfo.pci->subdevice_id);
  }
  else if (device->bustype == DRM_BUS_USB)
  {
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -     usb:", __FUNCTION__);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -       bus: {:#03}", __FUNCTION__,
              device->businfo.usb->bus);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -       dev: {:#03}", __FUNCTION__,
              device->businfo.usb->dev);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -   deviceinfo:", __FUNCTION__);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -     usb:", __FUNCTION__);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -       vendor:  {:#04x}", __FUNCTION__,
              device->deviceinfo.usb->vendor);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -       product: {:#04x}", __FUNCTION__,
              device->deviceinfo.usb->product);
  }
  else if (device->bustype == DRM_BUS_PLATFORM)
  {
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -     platform:", __FUNCTION__);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -       fullname: {}", __FUNCTION__,
              device->businfo.platform->fullname);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -   deviceinfo:", __FUNCTION__);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -     platform:", __FUNCTION__);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -       compatible:", __FUNCTION__);

    auto compatible = device->deviceinfo.platform->compatible;
    while (*compatible)
    {
      CLog::Log(LOGDEBUG, "CDRMUtils::{} -         {}:", __FUNCTION__, *compatible);
      compatible++;
    }
  }
  else if (device->bustype == DRM_BUS_HOST1X)
  {
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -     host1x:", __FUNCTION__);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -       fullname: {}", __FUNCTION__,
              device->businfo.host1x->fullname);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -   deviceinfo:", __FUNCTION__);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -     host1x:", __FUNCTION__);
    CLog::Log(LOGDEBUG, "CDRMUtils::{} -       compatible:", __FUNCTION__);

    auto compatible = device->deviceinfo.host1x->compatible;
    while (*compatible)
    {
      CLog::Log(LOGDEBUG, "CDRMUtils::{} -         {}:", __FUNCTION__, *compatible);
      compatible++;
    }
  }
  else
    CLog::Log(LOGDEBUG, "CDRMUtils::{} - unhandled bus type", __FUNCTION__);
}

bool CDRMUtils::OpenDrm(bool needConnector)
{
  int numDevices = drmGetDevices2(0, nullptr, 0);
  if (numDevices <= 0)
  {
    CLog::Log(LOGERROR, "CDRMUtils::{} - no drm devices found: ({})", __FUNCTION__,
              strerror(errno));
    return false;
  }

  CLog::Log(LOGDEBUG, "CDRMUtils::{} - drm devices found: {}", __FUNCTION__, numDevices);

  std::vector<drmDevicePtr> devices(numDevices);

  int ret = drmGetDevices2(0, devices.data(), devices.size());
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "CDRMUtils::{} - drmGetDevices2 return an error: ({})", __FUNCTION__,
              strerror(errno));
    return false;
  }

  for (const auto device : devices)
  {
    for (int i = 0; i < DRM_NODE_MAX; i++)
    {
      if (device->available_nodes & 1 << i)
      {
        CLog::Log(LOGDEBUG, "CDRMUtils::{} - opening device: {}", __FUNCTION__, device->nodes[i]);
        PrintDrmDeviceInfo(device);

        close(m_fd);
        m_fd = open(device->nodes[i], O_RDWR | O_CLOEXEC);
        if (m_fd < 0)
          continue;

        if (needConnector)
        {
          auto resources = drmModeGetResources(m_fd);
          if (!resources)
            continue;

          for (int i = 0; i < resources->count_connectors; i++)
          {
            auto connector = std::make_unique<CDRMConnector>(m_fd, resources->connectors[i]);
            if (connector->GetEncoderId() > 0 && connector->IsConnected())
              break;
          }

          drmModeFreeResources(resources);
        }

        CLog::Log(LOGDEBUG, "CDRMUtils::{} - opened device: {}", __FUNCTION__, device->nodes[i]);

        const char* renderPath = drmGetRenderDeviceNameFromFd(m_fd);
        if (renderPath)
        {
          m_renderFd = open(renderPath, O_RDWR | O_CLOEXEC);
          if (m_renderFd != 0)
            CLog::Log(LOGDEBUG, "CDRMUtils::{} - opened render node: {}", __FUNCTION__, renderPath);
        }

        drmFreeDevices(devices.data(), devices.size());
        return true;
      }
    }
  }

  drmFreeDevices(devices.data(), devices.size());
  return false;
}

bool CDRMUtils::InitDrm()
{
  if (m_fd < 0)
    return false;

  /* caps need to be set before allocating connectors, encoders, crtcs, and planes */
  int ret = drmSetClientCap(m_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  if (ret)
  {
    CLog::Log(LOGERROR, "CDRMUtils::{} - failed to set universal planes capability: {}",
              __FUNCTION__, strerror(errno));
    return false;
  }

  ret = drmSetClientCap(m_fd, DRM_CLIENT_CAP_STEREO_3D, 1);
  if (ret)
  {
    CLog::Log(LOGERROR, "CDRMUtils::{} - failed to set stereo 3d capability: {}", __FUNCTION__,
              strerror(errno));
    return false;
  }

#if defined(DRM_CLIENT_CAP_ASPECT_RATIO)
  ret = drmSetClientCap(m_fd, DRM_CLIENT_CAP_ASPECT_RATIO, 0);
  if (ret != 0)
    CLog::Log(LOGERROR, "CDRMUtils::{} - aspect ratio capability is not supported: {}",
              __FUNCTION__, strerror(errno));
#endif

  auto resources = drmModeGetResources(m_fd);
  if (!resources)
    return false;

  for (int i = 0; i < resources->count_connectors; i++)
    m_connectors.emplace_back(std::make_unique<CDRMConnector>(m_fd, resources->connectors[i]));

  for (int i = 0; i < resources->count_encoders; i++)
    m_encoders.emplace_back(std::make_unique<CDRMEncoder>(m_fd, resources->encoders[i]));

  for (int i = 0; i < resources->count_crtcs; i++)
    m_crtcs.emplace_back(std::make_unique<CDRMCrtc>(m_fd, resources->crtcs[i]));

  drmModeFreeResources(resources);

  auto planeResources = drmModeGetPlaneResources(m_fd);
  if (!planeResources)
    return false;

  for (uint32_t i = 0; i < planeResources->count_planes; i++)
    m_planes.emplace_back(std::make_unique<CDRMPlane>(m_fd, planeResources->planes[i]));

  for (auto& plane : m_planes)
  {
    if (!plane->FindModifiers())
    {
      if (plane->SupportsFormat(DRM_FORMAT_ARGB8888))
        plane->GetModifiersForFormat(DRM_FORMAT_ARGB8888)->emplace_back(DRM_FORMAT_MOD_LINEAR);

      if (plane->SupportsFormat(DRM_FORMAT_XRGB8888))
        plane->GetModifiersForFormat(DRM_FORMAT_XRGB8888)->emplace_back(DRM_FORMAT_MOD_LINEAR);
    }
  }

  drmModeFreePlaneResources(planeResources);

  if (!FindConnector())
    return false;

  if (!FindEncoder())
    return false;

  if (!FindCrtc())
    return false;

  if (!FindPlanes())
    return false;

  if (!FindPreferredMode())
    return false;

  ret = drmSetMaster(m_fd);
  if (ret < 0)
  {
    CLog::Log(LOGDEBUG,
              "CDRMUtils::{} - failed to set drm master, will try to authorize instead: {}",
              __FUNCTION__, strerror(errno));

    drm_magic_t magic;

    ret = drmGetMagic(m_fd, &magic);
    if (ret < 0)
    {
      CLog::Log(LOGERROR, "CDRMUtils::{} - failed to get drm magic: {}", __FUNCTION__,
                strerror(errno));
      return false;
    }

    ret = drmAuthMagic(m_fd, magic);
    if (ret < 0)
    {
      CLog::Log(LOGERROR, "CDRMUtils::{} - failed to authorize drm magic: {}", __FUNCTION__,
                strerror(errno));
      return false;
    }

    CLog::Log(LOGINFO, "CDRMUtils::{} - successfully authorized drm magic", __FUNCTION__);
  }

  return true;
}

bool CDRMUtils::FindConnector()
{
  auto connector = std::find_if(m_connectors.begin(), m_connectors.end(), [this](auto& connector) {
    return connector->GetEncoderId() > 0 && connector->IsConnected();
  });
  if (connector == m_connectors.end())
    return false;

  m_connector = connector->get();
  return true;
}

bool CDRMUtils::FindEncoder()
{
  auto encoder = std::find_if(m_encoders.begin(), m_encoders.end(), [this](auto& encoder) {
    return encoder->GetEncoderId() == m_connector->GetEncoderId();
  });
  if (encoder == m_encoders.end())
    return false;

  m_encoder = encoder->get();
  return true;
}

bool CDRMUtils::FindCrtc()
{
  for (size_t i = 0; i < m_crtcs.size(); i++)
  {
    if (m_encoder->GetPossibleCrtcs() & (1 << i))
    {
      if (m_crtcs[i]->GetCrtcId() == m_encoder->GetCrtcId())
      {
        m_orig_crtc = m_crtcs[i].get();
        return true;
      }
    }
  }

  return false;
}

bool CDRMUtils::RestoreOriginalMode()
{
  if(!m_orig_crtc)
  {
    return false;
  }

  auto ret = drmModeSetCrtc(m_fd, m_orig_crtc->GetCrtcId(), m_orig_crtc->GetBufferId(),
                            m_orig_crtc->GetX(), m_orig_crtc->GetY(), m_connector->GetConnectorId(),
                            1, m_orig_crtc->GetMode());

  if(ret)
  {
    CLog::Log(LOGERROR, "CDRMUtils::{} - failed to set original crtc mode", __FUNCTION__);
    return false;
  }

  CLog::Log(LOGDEBUG, "CDRMUtils::{} - set original crtc mode", __FUNCTION__);

  return true;
}

void CDRMUtils::DestroyDrm()
{
  RestoreOriginalMode();

  if (drmAuthMagic(m_fd, 0) == EINVAL)
    drmDropMaster(m_fd);

  close(m_renderFd);
  close(m_fd);

  m_connector = nullptr;
  m_encoder = nullptr;
  m_crtc = nullptr;
  m_orig_crtc = nullptr;
  m_video_plane = nullptr;
  m_gui_plane = nullptr;
}

RESOLUTION_INFO CDRMUtils::GetResolutionInfo(drmModeModeInfoPtr mode)
{
  RESOLUTION_INFO res;
  res.iScreenWidth = mode->hdisplay;
  res.iScreenHeight = mode->vdisplay;
  res.iWidth = res.iScreenWidth;
  res.iHeight = res.iScreenHeight;

  int limit = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
      SETTING_VIDEOSCREEN_LIMITGUISIZE);
  if (limit > 0 && res.iScreenWidth > 1920 && res.iScreenHeight > 1080)
  {
    switch (limit)
    {
      case 1: // 720p
        res.iWidth = 1280;
        res.iHeight = 720;
        break;
      case 2: // 1080p / 720p (>30hz)
        res.iWidth = mode->vrefresh > 30 ? 1280 : 1920;
        res.iHeight = mode->vrefresh > 30 ? 720 : 1080;
        break;
      case 3: // 1080p
        res.iWidth = 1920;
        res.iHeight = 1080;
        break;
      case 4: // Unlimited / 1080p (>30hz)
        res.iWidth = mode->vrefresh > 30 ? 1920 : res.iScreenWidth;
        res.iHeight = mode->vrefresh > 30 ? 1080 : res.iScreenHeight;
        break;
    }
  }

  if (mode->clock % 5 != 0)
    res.fRefreshRate = static_cast<float>(mode->vrefresh) * (1000.0f/1001.0f);
  else
    res.fRefreshRate = mode->vrefresh;
  res.iSubtitles = static_cast<int>(0.965 * res.iHeight);
  res.fPixelRatio = 1.0f;
  res.bFullScreen = true;

  if (mode->flags & DRM_MODE_FLAG_3D_MASK)
  {
    if (mode->flags & DRM_MODE_FLAG_3D_TOP_AND_BOTTOM)
      res.dwFlags = D3DPRESENTFLAG_MODE3DTB;
    else if (mode->flags & DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF)
      res.dwFlags = D3DPRESENTFLAG_MODE3DSBS;
  }
  else if (mode->flags & DRM_MODE_FLAG_INTERLACE)
    res.dwFlags = D3DPRESENTFLAG_INTERLACED;
  else
    res.dwFlags = D3DPRESENTFLAG_PROGRESSIVE;

  res.strMode = StringUtils::Format("%dx%d%s @ %.6f Hz", res.iScreenWidth, res.iScreenHeight,
                                    res.dwFlags & D3DPRESENTFLAG_INTERLACED ? "i" : "", res.fRefreshRate);
  return res;
}

RESOLUTION_INFO CDRMUtils::GetCurrentMode()
{
  return GetResolutionInfo(m_mode);
}

std::vector<RESOLUTION_INFO> CDRMUtils::GetModes()
{
  std::vector<RESOLUTION_INFO> resolutions;
  resolutions.reserve(m_connector->GetModesCount());

  for (auto i = 0; i < m_connector->GetModesCount(); i++)
  {
    RESOLUTION_INFO res = GetResolutionInfo(m_connector->GetModeForIndex(i));
    res.strId = std::to_string(i);
    resolutions.push_back(res);
  }

  return resolutions;
}

uint32_t CDRMUtils::FourCCWithAlpha(uint32_t fourcc)
{
  return (fourcc & 0xFFFFFF00) | static_cast<uint32_t>('A');
}

uint32_t CDRMUtils::FourCCWithoutAlpha(uint32_t fourcc)
{
  return (fourcc & 0xFFFFFF00) | static_cast<uint32_t>('X');
}

std::string CDRMUtils::FourCCToString(uint32_t fourcc)
{
  std::stringstream ss;
  ss << static_cast<char>((fourcc & 0x000000FF));
  ss << static_cast<char>((fourcc & 0x0000FF00) >> 8);
  ss << static_cast<char>((fourcc & 0x00FF0000) >> 16);
  ss << static_cast<char>((fourcc & 0xFF000000) >> 24);

  return ss.str();
}