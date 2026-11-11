#pragma once
#include "InstallSupport/GlassPackageAcquisition.h"
#include "InstallSupport/GlassSecureUxPackage.h"
#include "InstallSupport/GlassOpenGlassPackage.h"
#include "InstallSupport/GlassReleaseClient.h"
#include "InstallSupport/GlassSetupSourceLayout.h"
#include "InstallSupport/GlassMaintenanceCache.h"
#include <thread>
#include <memory>
#include <vector>
#include <array>
#include <algorithm>
#include <filesystem>
#include <objbase.h>
#include "InstallLog.h"

// UI thread owns selection/generations. Workers never touch HWND, selection or
// the published hints. A cancelled generation cannot publish into a newer one.
class GlassProductDownloads {
public:
    struct Job {
        std::atomic<bool> cancel{false},done{false},network{false};
        std::atomic<std::uint64_t> bytes{0},total{0};
        std::thread thread;
        std::wstring directory,path,error;
        bool ok=false,published=false;
        ULONGLONG started=GetTickCount64();
        ~Job(){cancel=true;if(thread.joinable())thread.join();if(!directory.empty())GlassMaintenanceCache::RemoveFlat(directory);}
    };
    std::array<std::shared_ptr<Job>,6> jobs{};
    std::vector<std::shared_ptr<Job>> retired;
    bool stopped=false;
    ~GlassProductDownloads(){Stop();Join();}
    void Stop() {
        stopped=true;
        for(std::size_t i=0;i<jobs.size();++i){GlassSetupSourceLayout::SetPrepared(i,{});if(jobs[i])jobs[i]->cancel=true;}
        for(auto& job:retired)job->cancel=true;
    }
    void Retry(std::size_t i) {
        if(i<jobs.size()&&jobs[i]&&jobs[i]->done.load()&&!jobs[i]->ok) {
            GlassSetupSourceLayout::SetPrepared(i,{});retired.push_back(std::move(jobs[i]));
        }
    }
    void Join() {for(auto& job:jobs)job.reset();retired.clear();}
    bool Ready(const std::array<bool,6>& selected) const {
        for(std::size_t i=0;i<jobs.size();++i)if(selected[i]&&(!jobs[i]||!jobs[i]->done.load()||!jobs[i]->ok||!jobs[i]->published||jobs[i]->cancel.load()))return false;
        return true;
    }
    void Reconcile(const std::array<bool,6>& selected,const GlassPackageCatalog::Catalog& catalog) {
        if(stopped)return;
        retired.erase(std::remove_if(retired.begin(),retired.end(),[](const auto& job){return job->done.load();}),retired.end());
        for(std::size_t i=0;i<jobs.size();++i) {
            auto& job=jobs[i];
            if(!selected[i]) {
                if(job){job->cancel=true;GlassSetupSourceLayout::SetPrepared(i,{});retired.push_back(std::move(job));}
                continue;
            }
            if(job) {
                if(job->done.load()&&job->ok&&!job->cancel.load()&&!job->published){GlassSetupSourceLayout::SetPrepared(i,job->path);job->published=true;}
                continue;
            }
            job=std::make_shared<Job>();auto current=job.get();
            if(i<5&&catalog.products[i])job->total=catalog.products[i]->size;
            try {
                job->thread=std::thread([current,catalog,i]() {
                    InstallLog::LineFormat(L"Product preparation: start index=%u",static_cast<unsigned>(i));
                    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
                    try {
                        GlassReleaseClient::packageProgress=[current,i](std::uint64_t bytes,std::uint64_t total) {
                            if(!current->network.exchange(true))
                                InstallLog::LineFormat(L"Product preparation: GitHub package transfer started index=%u expectedBytes=%llu",
                                    static_cast<unsigned>(i),static_cast<unsigned long long>(total));
                            current->total=total;current->bytes=bytes;
                        };
                        if(FAILED(com))current->error=L"COM initialization failed";
                        else if(!GlassPackageAcquisition::CreateWorkspace(current->directory))current->error=L"Cannot create protected package workspace";
                        else if(i==4) {
                            GlassOpenGlassPackage::Package package;
                            current->ok=GlassOpenGlassPackage::PrepareForCurrentSystem(current->directory,current->cancel,package,current->error);
                            current->path=package.setup;
                        }
                        else if(i<4) {
                            current->ok=GlassPackageAcquisition::Prepare(static_cast<GlassPackageCatalog::Product>(i),catalog,current->directory,current->cancel,current->error);
                            if(current->ok)current->path=current->directory+L"\\"+GlassReleaseClient::Wide(GlassPackageCatalog::kFiles[i]);
                        } else {
                            GlassSecureUxPackage::Package package;
                            current->ok=GlassSecureUxPackage::Prepare(current->directory,current->cancel,package,current->error);
                            current->path=package.path;
                        }
                        if(current->ok&&!current->network.load()&&!current->path.empty()) {
                            std::error_code ec;const auto size=std::filesystem::file_size(current->path,ec);
                            if(!ec)current->total=static_cast<std::uint64_t>(size);
                        }
                    }catch(...){current->ok=false;current->error=L"Cannot prepare product package";}
                    GlassReleaseClient::packageProgress={};
                    if(SUCCEEDED(com))CoUninitialize();
                    InstallLog::LineFormat(L"Product preparation: finish index=%u ok=%d cancelled=%d network=%d bytes=%llu detail=%s",
                        static_cast<unsigned>(i),current->ok?1:0,current->cancel.load()?1:0,current->network.load()?1:0,
                        static_cast<unsigned long long>(current->bytes.load()),current->error.c_str());
                    current->done.store(true,std::memory_order_release);
                });
            }catch(...){job->error=L"Cannot start package worker";job->done=true;}
        }
    }
};
