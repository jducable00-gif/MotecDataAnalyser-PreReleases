#pragma once
// TabletReportServer lifecycle + payload publishing, extracted from MotecSetupForm.cpp so it can be unit-tested without VCL.

#include <string>
#include "HttpServer.h"
#include "MotecSetupAnalyzer.h"   // LapStats, ReportResult (via LapReportGenerator)

class TabletPublisher {
public:
    explicit TabletPublisher(int defaultPort = 8765);

    bool isRunning() const;
    void stopServer();                 // no-op if not running
    void setPort(int port);            // applied on next start while stopped (TabletReportServer semantics)
    int activePort() const;
    bool startServer();                // TabletReportServer::start shows its own error dialog on failure

    // Live status frame: car/track/lap meta only, no report sections.
    void publishLiveStatus(const std::string& vehicleNameUtf8,
                           const std::string& trackLocationUtf8, int lapCount);

    // Full averaged-lap report payload (version embedded in the JSON).
    void publishReport(const ReportResult& result, const std::vector<LapStats>& laps);

private:
    TabletReportServer server_;
    int version_ = 1;                  // incremented per publish, sent as "version" so tablets can ignore stale frames
};
