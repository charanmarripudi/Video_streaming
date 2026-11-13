CXX ?= clang++
CXXFLAGS = -std=c++20 -O2 -Wall -Wextra -pthread -Iinclude -Ithird_party

# On macOS with Apple Clang, enable experimental library for jthread/stop_token
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
	CXXFLAGS += -fexperimental-library
endif

TARGET = bin/video_engine

SRCS = src/config/config_manager.cpp \
       src/config/config_watcher.cpp \
       src/capture/stream_supervisor.cpp \
       src/capture/stream_capture.cpp \
       src/inference/inference_service.cpp \
       src/inference/use_case_worker.cpp \
       src/recorder/event_manager.cpp \
       src/orchestrator/camera_context.cpp \
       src/orchestrator/orchestrator.cpp \
       src/main.cpp

OBJS = $(SRCS:.cpp=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p bin storage/events
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS)
	@echo "Build successful! v2 Production Binary created at $(TARGET)"

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET) config.json

clean:
	rm -rf bin $(OBJS) storage/events/*.mp4 storage/events/*.tmp

.PHONY: all run clean
