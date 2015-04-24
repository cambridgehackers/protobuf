# See README.txt.

.PHONY: all cpp clean

all: cpp bsv

cpp:    add_person_cpp    list_people_cpp
bsv: add_person_bsv list_people_bsv

clean:
	rm -f add_person_cpp list_people_cpp
	rm -f protoc_middleman addressbook.pb.cc addressbook.pb.h addressbook_pb2.bsv
	rm -f *.pyc

protoc_middleman: addressbook.proto ../src/protoc
	../src/protoc --cpp_out=. --bsv_out=. addressbook.proto
	#@touch protoc_middleman

add_person_cpp: add_person.cc protoc_middleman
	pkg-config --cflags protobuf  # fails if protobuf is not installed
	c++ add_person.cc addressbook.pb.cc -o add_person_cpp `pkg-config --cflags --libs protobuf`

list_people_cpp: list_people.cc protoc_middleman
	pkg-config --cflags protobuf  # fails if protobuf is not installed
	c++ list_people.cc addressbook.pb.cc -o list_people_cpp `pkg-config --cflags --libs protobuf`

add_person_bsv: protoc_middleman

#@echo "Writing shortcut script add_person_bsv..."
#@echo '#! /bin/sh' > add_person_bsv
#@echo './add_person.py "$$@"' >> add_person_bsv
#@chmod +x add_person_bsv

list_people_bsv: protoc_middleman

#@echo "Writing shortcut script list_people_bsv..."
#@echo '#! /bin/sh' > list_people_bsv
#@echo './list_people.py "$$@"' >> list_people_bsv
#@chmod +x list_people_bsv
