document.addEventListener("DOMContentLoaded", () => {
    const callBtn = document.getElementById("callBtn");
    const stopBtn = document.getElementById("stopBtn");
    const statusBox = document.getElementById("statusBox");

    if (!callBtn || !stopBtn || !statusBox) {
        console.error("[LibBot] Missing required DOM elements.");
        return;
    }

    function setStatus(message, isError = false) {
        statusBox.innerText = message;
        statusBox.className = isError
            ? "mt-5 text-sm text-red-600 min-h-[48px]"
            : "mt-5 text-sm text-slate-600 min-h-[48px]";
    }

    async function parseJsonSafely(response) {
        try {
            return await response.json();
        } catch {
            return {};
        }
    }

    callBtn.addEventListener("click", async () => {
        const tableId = parseInt(callBtn.dataset.tableId, 10);

        if (!Number.isInteger(tableId)) {
            setStatus("Lỗi: không xác định được số bàn.", true);
            return;
        }

        callBtn.disabled = true;
        callBtn.innerText = "Đang gửi yêu cầu...";
        setStatus("Trạng thái: đang gửi yêu cầu đến hệ thống.");

        try {
            const response = await fetch("/api/robot/call/", {
                method: "POST",
                headers: {
                    "Content-Type": "application/json"
                },
                body: JSON.stringify({
                    table_id: tableId
                })
            });

            const data = await parseJsonSafely(response);

            if (response.status === 409) {
                throw new Error(data.error || "LibBot đang bận, vui lòng thử lại sau.");
            }

            if (!response.ok || !data.ok) {
                throw new Error(data.error || "Không thể gọi robot.");
            }

            setStatus(data.message || `Đã gọi LibBot đến bàn ${tableId}.`);
            callBtn.innerText = "Đã gửi yêu cầu";

        } catch (error) {
            setStatus("Lỗi: " + error.message, true);
            callBtn.disabled = false;
            callBtn.innerText = "Gọi lại LibBot";
        }
    });

    stopBtn.addEventListener("click", async () => {
        const confirmed = confirm("Dừng khẩn cấp LibBot?");
        if (!confirmed) return;

        stopBtn.disabled = true;
        stopBtn.innerText = "Đang gửi lệnh dừng...";
        setStatus("Trạng thái: đang gửi lệnh dừng khẩn cấp.");

        try {
            const response = await fetch("/api/robot/stop/", {
                method: "POST",
                headers: {
                    "Content-Type": "application/json"
                }
            });

            const data = await parseJsonSafely(response);

            if (!response.ok || !data.ok) {
                throw new Error(data.error || "Không thể dừng robot.");
            }

            setStatus("Đã gửi lệnh dừng khẩn cấp.");
            stopBtn.innerText = "Đã dừng";

            callBtn.disabled = false;
            callBtn.innerText = "Gọi LibBot đến bàn này";

        } catch (error) {
            setStatus("Lỗi dừng khẩn cấp: " + error.message, true);
            stopBtn.disabled = false;
            stopBtn.innerText = "Dừng khẩn cấp";
        }
    });
});