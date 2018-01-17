#include "pch.h"

#include <Kore/IO/FileReader.h>
#include <Kore/Math/Core.h>
#include <Kore/System.h>
#include <Kore/Input/Keyboard.h>
#include <Kore/Input/Mouse.h>
#include <Kore/Graphics1/Image.h>
#include <Kore/Graphics4/Graphics.h>
#include <Kore/Graphics4/PipelineState.h>
#include <Kore/Threads/Thread.h>
#include <Kore/Threads/Mutex.h>
#include "ObjLoader.h"

using namespace Kore;

namespace {
	Mutex streamMutex;
}

class MeshData {
public:
	MeshData(const char* meshFile, const Graphics4::VertexStructure& structure) {
		mesh = loadObj(meshFile);
		
		vertexBuffer = new Graphics4::VertexBuffer(mesh->numVertices, structure);
		float* vertices = vertexBuffer->lock();
		for (int i = 0; i < mesh->numVertices; ++i) {
			vertices[i * 8 + 0] = mesh->vertices[i * 8 + 0];
			vertices[i * 8 + 1] = mesh->vertices[i * 8 + 1];
			vertices[i * 8 + 2] = mesh->vertices[i * 8 + 2];
			vertices[i * 8 + 3] = mesh->vertices[i * 8 + 3];
			vertices[i * 8 + 4] = 1.0f - mesh->vertices[i * 8 + 4];
			vertices[i * 8 + 5] = mesh->vertices[i * 8 + 5];
			vertices[i * 8 + 6] = mesh->vertices[i * 8 + 6];
			vertices[i * 8 + 7] = mesh->vertices[i * 8 + 7];
		}
		vertexBuffer->unlock();

		indexBuffer = new Graphics4::IndexBuffer(mesh->numFaces * 3);
		int* indices = indexBuffer->lock();
		for (int i = 0; i < mesh->numFaces * 3; i++) {
			indices[i] = mesh->indices[i];
		}
		indexBuffer->unlock();
	}

	Graphics4::VertexBuffer* vertexBuffer;
	Graphics4::IndexBuffer* indexBuffer;
	Mesh* mesh;
};

class MeshObject {
public:
	MeshObject(MeshData* mesh, Graphics4::Texture* image, vec3 position) : mesh(mesh), image(image) {
		M = mat4::Translation(position.x(), position.y(), position.z());
		isLowRes = (image->height <= 16);
		preImage = nullptr;
	}

	void render(Graphics4::TextureUnit tex) {
		Graphics4::setTexture(tex, image);
		Graphics4::setVertexBuffer(*mesh->vertexBuffer);
		Graphics4::setIndexBuffer(*mesh->indexBuffer);
		Graphics4::drawIndexedVertices();
	}

	void setTexture(Graphics4::Texture* tex) {
		delete image;
		image = tex;
	}

	Graphics4::Texture* getTexture() {
		return image;
	}

	void setImage() {
		streamMutex.lock();
		if (preImage != nullptr) {
			// create new texture to assign preImage data and overwrite old texture
			Graphics4::Texture* nextImage = new Graphics4::Texture(preImage->width, preImage->height, preImage->format, preImage->readable);
			u8* data = nextImage->lock();
			// assign correct colors RGBA (from image) -> BGRA (from texture)
			for (int i = 0; i < preImage->width * preImage->height * 4; i += 4) {
				data[i] = preImage->data[i + 2];
				data[i + 1] = preImage->data[i + 1];
				data[i + 2] = preImage->data[i];
				data[i + 3] = preImage->data[i + 3];
			}
			nextImage->unlock();
			setTexture(nextImage);
			delete preImage;
			// do not repeat this every time
			preImage = nullptr;
		}
		streamMutex.unlock();
	}

	mat4 M;
	Graphics4::Image* preImage;
	bool isLowRes;
private:
	MeshData* mesh;
	Graphics4::Texture* image;
};

namespace {
	const int width = 1024;
	const int height = 768;
	double startTime;
	Graphics4::Shader* vertexShader;
	Graphics4::Shader* fragmentShader;
	Graphics4::PipelineState* pipeline;

	// null terminated array of MeshObject pointers
	MeshObject** objects;

	// The view projection matrix aka the camera
	mat4 P;
	mat4 V;

	vec3 position;
	
	// vertical angle of fov
	float fov = 60.0f;

	// approximation of horizontal angle of fov (because the formula is easier than 2*atan(tan(fov)*width/height))
	float hfov = (float)fov * width / height;
	
	bool up = false, down = false, left = false, right = false;

	Thread* streamingThread;

	// uniform locations - add more as you see fit
	Graphics4::TextureUnit tex;
	Graphics4::ConstantLocation pLocation;
	Graphics4::ConstantLocation vLocation;
	Graphics4::ConstantLocation mLocation;

	float angle;

	void stream(void*) {
		for (;;) {
			// iterate the MeshObjects
			MeshObject** currentPtr = &objects[0];
			// load new preImages for every box if necessary
			while (*currentPtr != nullptr) {
				MeshObject* current = (*currentPtr);
				// read the position dependent on the current view (in camera coordinates)
				mat4 VM = V*current->M;
				// zPos with an offset of 3 because the position depends on the center of the box and we want to know when the object is fully behind the camera
				float zPos = VM.get(2, 3) + 3;
				// get cosine of view angle to box (as to kick out higher resolution images outside the fov)
				vec3 camViewDir = vec3(0, 0, 1);
				vec3 objVec = vec3(VM.get(0, 3), VM.get(1, 3), zPos);
				objVec.normalize();
				float cosine = camViewDir * objVec;
				// higher resolution image if box is near enough and not outside the approximate horizontal fov of the camera
				if (zPos < 40 && cosine >= Kore::cos((float)(hfov / 2) / 180.0f * pi)) {
					if (current->isLowRes) {
						Graphics4::Image* nextImage = new Graphics4::Image("darmstadt.jpg", true);
						streamMutex.lock();
						delete current->preImage;
						current->preImage = nextImage;
						current->isLowRes = false;
						streamMutex.unlock();
					}
				}
				else {
					// kick out higher resolution (preImage will be overwritten)
					if (!current->isLowRes) {
						Graphics4::Image* nextImage = new Graphics4::Image("darmstadtmini.png", true);
						streamMutex.lock();
						delete current->preImage;
						current->preImage = nextImage;
						current->isLowRes = true;
						streamMutex.unlock();
					}
				}
				++currentPtr;
			}
		}
	}

	void update() {
		float t = (float)(System::time() - startTime);
		
		MeshObject** current = &objects[0];

		Graphics4::begin();
		Graphics4::clear(Graphics4::ClearColorFlag | Graphics4::ClearDepthFlag, 0xff9999FF, 1.0f);

		// Update textures, if needed
		while (*current != nullptr) {
			(*current)->setImage();
			++current;
		}

		const float speed = 0.1f;
		if (up) position.z() += speed;
		if (down) position.z() -= speed;
		if (left) position.x() -= speed;
		if (right) position.x() += speed;
		
		Graphics4::setPipeline(pipeline);

		// set the camera
		P = mat4::Perspective(pi / 4.0f, (float)width / (float)height, 0.1f, 100);
		V = mat4::lookAt(position, vec3(0, 0, 1000), vec3(0, 1, 0));
		Graphics4::setMatrix(pLocation, P);
		Graphics4::setMatrix(vLocation, V);


		angle = t;


		//objects[0]->M = mat4::RotationY(angle) * mat4::RotationZ(Kore::pi / 4.0f);


		// iterate the MeshObjects
		current = &objects[0];
		while (*current != nullptr) {
			// set the model matrix
			Graphics4::setMatrix(mLocation, (*current)->M);

			(*current)->render(tex);
			++current;
		}

		Graphics4::end();
		Graphics4::swapBuffers();
	}

	void mouseMove(int windowId, int x, int y, int movementX, int movementY) {

	}
	
	void mousePress(int windowId, int button, int x, int y) {

	}

	void mouseRelease(int windowId, int button, int x, int y) {

	}

	void keyDown(KeyCode code) {
		switch (code) {
		case KeyLeft:
			left = true;
			break;
		case KeyRight:
			right = true;
			break;
		case KeyUp:
			up = true;
			break;
		case KeyDown:
			down = true;
			break;
		}
	}

	void keyUp(KeyCode code) {
		switch (code) {
		case KeyLeft:
			left = false;
			break;
		case KeyRight:
			right = false;
			break;
		case KeyUp:
			up = false;
			break;
		case KeyDown:
			down = false;
			break;
		}
	}


	void init() {
		FileReader vs("shader.vert");
		FileReader fs("shader.frag");
		vertexShader = new Graphics4::Shader(vs.readAll(), vs.size(), Graphics4::VertexShader);
		fragmentShader = new Graphics4::Shader(fs.readAll(), fs.size(), Graphics4::FragmentShader);

		// This defines the structure of your Vertex Buffer
		Graphics4::VertexStructure structure;
		structure.add("pos", Graphics4::Float3VertexData);
		structure.add("tex", Graphics4::Float2VertexData);
		structure.add("nor", Graphics4::Float3VertexData);

		pipeline = new Graphics4::PipelineState;
		pipeline->inputLayout[0] = &structure;
		pipeline->inputLayout[1] = nullptr;
		pipeline->vertexShader = vertexShader;
		pipeline->fragmentShader = fragmentShader;
		pipeline->depthMode = Graphics4::ZCompareLess;
		pipeline->depthWrite = true;
		pipeline->compile();

		tex = pipeline->getTextureUnit("tex");
		pLocation = pipeline->getConstantLocation("P");
		vLocation = pipeline->getConstantLocation("V");
		mLocation = pipeline->getConstantLocation("M");

		objects = new MeshObject*[101];
		for (int i = 0; i < 101; ++i) objects[i] = nullptr;

		MeshData* mesh = new MeshData("box.obj", structure);
		for (int y = 0; y < 10; ++y) {
			for (int x = 0; x < 10; ++x) {
				objects[y * 10 + x] = new MeshObject(mesh, new Graphics4::Texture("darmstadtmini.png", true), vec3((x - 5.0f) * 10, 0, (y - 5.0f) * 10));
			}
		}

		angle = 0.0f;

		Graphics4::setTextureAddressing(tex, Graphics4::U, Graphics4::Repeat);
		Graphics4::setTextureAddressing(tex, Graphics4::V, Graphics4::Repeat);
	}
}

int kore(int argc, char** argv) {
	Kore::System::init("Exercise 11", width, height);
	
	init();

	Kore::System::setCallback(update);

	startTime = System::time();

	Keyboard::the()->KeyDown = keyDown;
	Keyboard::the()->KeyUp = keyUp;
	Mouse::the()->Move = mouseMove;
	Mouse::the()->Press = mousePress;
	Mouse::the()->Release = mouseRelease;

	streamMutex.create();
	Kore::threadsInit();
	streamingThread = Kore::createAndRunThread(stream, nullptr);

	Kore::System::start();
	
	return 0;
}
