#include "FSScheduler.hpp"

enum class FSEventType {
	None,
	ButtonPress,
	ButtonRelease,
	TouchStart,
	TouchMove,
	TouchEnd,
};

class FSEvent {
	private:
	uint32_t cycle;
	FSEventType type;
};

class FSEvents {
	FSScheduler* scheduler;


	
	public:
	FSEvents(FSScheduler* schedsrc) {
		this->scheduler = schedsrc;
	}


};