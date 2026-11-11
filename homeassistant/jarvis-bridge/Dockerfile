FROM python:3.11-slim-bookworm
ENV PYTHONUNBUFFERED=1 HF_HUB_DISABLE_PROGRESS_BARS=1 HF_HUB_DISABLE_TELEMETRY=1
RUN apt-get update && apt-get install -y --no-install-recommends libgomp1 libstdc++6 ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY bridge.py usage.py run.sh LICENSE THIRD_PARTY_NOTICES.md ./
RUN chmod +x /app/run.sh
CMD ["/app/run.sh"]
