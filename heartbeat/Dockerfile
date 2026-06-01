FROM python:3.11-slim
WORKDIR /app

RUN pip install requests telebot anthropic schedule beautifulsoup4 pypdf

COPY monitor.py .

CMD ["python", "-u", "monitor.py"]
