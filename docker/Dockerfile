ARG PYTHON_VERSION

# Get the Python image of the version we're testing
FROM python:${PYTHON_VERSION}-slim

# Install dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    && rm -rf /var/lib/apt/lists/*

# Copy data to the container
COPY cmsgpack /usr/src/app/cmsgpack
COPY tests /usr/src/app/tests
COPY setup.py /usr/src/app/setup.py
COPY pyproject.toml /usr/src/app/pyproject.toml
COPY README.md /usr/src/app/README.md

WORKDIR /usr/src/app

# Install the module
RUN pip install . -v