#pragma once
// Best-effort UPnP port mapping for internet hosting behind strict NATs.
// The ICE agent is pinned to a known UDP port range (rtc::Configuration
// portRangeBegin/End); mapping that range on the IGD gives the NAT a stable,
// inbound-open binding, which STUN then observes and advertises -- this is what
// lets a host behind a symmetric NAT accept peers without TURN.
//
// Everything is best-effort and quiet: no IGD (router UPnP off, common) is one
// log line, never an error. All calls block on SOAP round-trips -- run map() on
// a background thread. unmapAll() removes exactly what map() added.
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace nn {

class UpnpMapper
{
public:
    // Discover the IGD and map [portBegin, portEnd] UDP. Returns ports mapped.
    int map(uint16_t portBegin, uint16_t portEnd)
    {
        int err = 0;
        UPNPDev *devs = upnpDiscover(2500, nullptr, nullptr, 0, 0, 2, &err);
        if (!devs)
        {
            printf("[nnet] upnp: nothing answered SSDP (err=%d)\n", err);
            fflush(stdout);
            return 0;
        }
        UPNPUrls urls;
        IGDdatas data;
        char lan[64] = {0}, wan[64] = {0};
        int r = UPNP_GetValidIGD(devs, &urls, &data, lan, sizeof lan, wan, sizeof wan);
        freeUPNPDevlist(devs);
        if (r != 1 && r != 2) // 1 = connected IGD; 2 = IGD with private WAN (double NAT: map anyway)
        {
            printf("[nnet] upnp: no usable IGD (r=%d; router UPnP likely off)\n", r);
            fflush(stdout);
            return 0;
        }
        char ext[64] = {0};
        UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, ext);

        int ok = 0;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            controlUrl_ = urls.controlURL;
            serviceType_ = data.first.servicetype;
            for (unsigned p = portBegin; p <= portEnd; ++p)
            {
                std::string port = std::to_string(p);
                int ar = UPNP_AddPortMapping(controlUrl_.c_str(), serviceType_.c_str(), port.c_str(),
                                             port.c_str(), lan, "eduke32-mp", "UDP", nullptr, "7200");
                if (ar == UPNPCOMMAND_SUCCESS)
                {
                    mapped_.push_back(port);
                    ++ok;
                }
            }
        }
        printf("[nnet] upnp: mapped %d/%d UDP ports %u-%u on %s (ext %s%s)\n", ok,
               portEnd - portBegin + 1, portBegin, portEnd, lan, ext[0] ? ext : "?",
               r == 2 ? ", double NAT: inbound may still be filtered upstream" : "");
        fflush(stdout);
        FreeUPNPUrls(&urls);
        return ok;
    }

    void unmapAll()
    {
        std::vector<std::string> ports;
        std::string ctrl, svc;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            ports.swap(mapped_);
            ctrl = controlUrl_;
            svc = serviceType_;
        }
        if (ports.empty() || ctrl.empty())
            return;
        int ok = 0;
        for (auto &p : ports)
            if (UPNP_DeletePortMapping(ctrl.c_str(), svc.c_str(), p.c_str(), "UDP", nullptr)
                == UPNPCOMMAND_SUCCESS)
                ++ok;
        printf("[nnet] upnp: unmapped %d/%zu ports\n", ok, ports.size());
        fflush(stdout);
    }

private:
    std::mutex mtx_;
    std::string controlUrl_, serviceType_;
    std::vector<std::string> mapped_;
};

} // namespace nn
