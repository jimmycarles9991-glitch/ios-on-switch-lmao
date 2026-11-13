// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

// IOSurfaceAccelerator requests waiting for VIC's engine thread. One whose events have not fired counts against its own
// connection, a runnable one against the shared bound, so a client whose events never fire stalls only itself. The
// caller holds one lock across every call.
template <class Request> class SwitchVICRequests {
public:
    static constexpr uint32_t kRunnable = 16, kWaitingPerConnection = 16;

    bool admits(const Request &request) const
    {
        return request.eventReady ? runnable < kRunnable : request.conn->waiting < kWaitingPerConnection;
    }

    void push(Request *request)
    {
        request->next = nullptr;
        request->listed = true;
        if (tail)
            tail->next = request;
        else
            head = request;
        tail = request;
        if (request->eventReady)
            ++runnable;
        else
            ++request->conn->waiting;
    }

    // Its events fired (status 0) or failed. Called before the request is listed, it only marks it.
    void ready(Request *request, int status)
    {
        if (!request->eventStatus)
            request->eventStatus = status;
        if (request->eventReady)
            return;
        request->eventReady = true;
        if (request->listed) {
            --request->conn->waiting;
            ++runnable;
        }
    }

    // Runs as a failure: the engine thread finishes it with status and gives VIC nothing.
    void fail(Request *request, int status)
    {
        request->eventStatus = status;
        ready(request, status);
    }

    template <class Connection> void cancel(const Connection *conn, int status)
    {
        for (auto *request = head; request; request = request->next)
            if (request->conn == conn)
                fail(request, status);
    }

    Request *pop()
    {
        Request *previous = nullptr;
        for (auto *request = head; request; previous = request, request = request->next) {
            if (!request->eventReady)
                continue;
            (previous ? previous->next : head) = request->next;
            if (tail == request)
                tail = previous;
            request->next = nullptr;
            request->listed = false;
            --runnable;
            return request;
        }
        return nullptr;
    }

private:
    Request *head = nullptr, *tail = nullptr;
    uint32_t runnable = 0;
};
