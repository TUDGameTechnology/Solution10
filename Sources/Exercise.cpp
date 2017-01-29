#include "pch.h"

#include <Kore/IO/FileReader.h>
#include <Kore/Math/Core.h>
#include <Kore/System.h>
#include <Kore/Input/Keyboard.h>
#include <Kore/Input/Mouse.h>
#include <Kore/Audio/Mixer.h>
#include <Kore/Graphics/Image.h>
#include <Kore/Graphics/Graphics.h>
#include <Kore/Threads/Thread.h>
#include <Kore/Threads/Mutex.h>
#include "ObjLoader.h"

using namespace Kore;

class MeshData {
public:
	MeshData(const char* meshFile, const VertexStructure& structure) {
		mesh = loadObj(meshFile);
		
		vertexBuffer = new VertexBuffer(mesh->numVertices, structure);
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

		indexBuffer = new IndexBuffer(mesh->numFaces * 3);
		int* indices = indexBuffer->lock();
		for (int i = 0; i < mesh->numFaces * 3; i++) {
			indices[i] = mesh->indices[i];
		}
		indexBuffer->unlock();
	}

	VertexBuffer* vertexBuffer;
	IndexBuffer* indexBuffer;
	Mesh* mesh;
};

class MeshObject {
public:
	MeshObject(MeshData* mesh, Texture* image, vec3 position) : mesh(mesh), image(image) {
		M = mat4::Translation(position.x(), position.y(), position.z());
		isLowRes = (image->height <= 16);
		preImage = nullptr;
	}

	void render(TextureUnit tex) {
		Graphics::setTexture(tex, image);
		Graphics::setVertexBuffer(*mesh->vertexBuffer);
		Graphics::setIndexBuffer(*mesh->indexBuffer);
		Graphics::drawIndexedVertices();
	}

	void setTexture(Texture* tex) {
		delete image;
		image = tex;
	}

	Texture* getTexture() {
		return image;
	}

	void setImage() {
		if (preImage != nullptr) {
			// create new texture to assign preImage data and overwrite old texture
			Texture* nextImage = new Texture(preImage->width, preImage->height, preImage->format, preImage->readable);
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
	}

	mat4 M;
	Kore::Image* preImage;
	bool isLowRes;
private:
	MeshData* mesh;
	Texture* image;
};

namespace {
	const int width = 1024;
	const int height = 768;
	double startTime;
	Shader* vertexShader;
	Shader* fragmentShader;
	Program* program;

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
	Mutex streamMutex;

	// uniform locations - add more as you see fit
	TextureUnit tex;
	ConstantLocation pLocation;
	ConstantLocation vLocation;
	ConstantLocation mLocation;

	float angle;

	void stream(void*) {
		for (;;) {
			// to use a mutex, create a Mutex variable and call Create to initialize the mutex (see main()). Then you can use Lock/Unlock.
			streamMutex.Lock();

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
						delete current->preImage;
						current->preImage = new Kore::Image("darmstadt.jpg", true);
						current->isLowRes = false;
					}
				}
				else {
					// kick out higher resolution (preImage will be overwritten)
					if (!current->isLowRes) {
						delete current->preImage;
						current->preImage = new Kore::Image("darmstadtmini.png", true);
						current->isLowRes = true;
					}
				}
				++currentPtr;
			}
			// load darmstadt.jpg files for near boxes
			// reload darmstadt.jpg for every box, pretend that every box has a different texture (I don't want to upload 100 images though)
			// feel free to create more versions of darmstadt.jpg at different sizes
			// always use less than 1 million pixels of texture data (the example code uses 100 16x16 textures - that's 25600 pixels, darmstadt.jpg is 512x512 aka 262144 pixels)

			// Beware, neither OpenGL nor Direct3D is thread safe - you can't just create a Texture in a second thread. But you can create a Kore::Image
			// in another thread, access it's pixels in the main thread and put them in a Kore::Texture using lock/unlock.

			streamMutex.Unlock();
		}
	}

	void update() {
		float t = (float)(System::time() - startTime);
		Kore::Audio::update();
		MeshObject** current = &objects[0];

		Graphics::begin();
		Graphics::clear(Graphics::ClearColorFlag | Graphics::ClearDepthFlag, 0xff9999FF, 1000.0f);

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
		
		Graphics::begin();
		Graphics::clear(Graphics::ClearColorFlag | Graphics::ClearDepthFlag, 0xff9999FF, 1.0f);
		
		program->set();

	
		// set the camera
		P = mat4::Perspective(60, (float)width / (float)height, 0.1f, 100);
		V = mat4::lookAt(position, vec3(0, 0, 1000), vec3(0, 1, 0));
		Graphics::setMatrix(pLocation, P);
		Graphics::setMatrix(vLocation, V);


		angle = t;


		//objects[0]->M = mat4::RotationY(angle) * mat4::RotationZ(Kore::pi / 4.0f);


		// iterate the MeshObjects
		current = &objects[0];
		while (*current != nullptr) {
			// set the model matrix
			Graphics::setMatrix(mLocation, (*current)->M);

			(*current)->render(tex);
			++current;
		}

		Graphics::end();
		Graphics::swapBuffers();
	}

	void mouseMove(int windowId, int x, int y, int movementX, int movementY) {

	}
	
	void mousePress(int windowId, int button, int x, int y) {

	}

	void mouseRelease(int windowId, int button, int x, int y) {

	}

	void keyDown(KeyCode code, wchar_t character) {
		switch (code) {
		case Key_Left:
			left = true;
			break;
		case Key_Right:
			right = true;
			break;
		case Key_Up:
			up = true;
			break;
		case Key_Down:
			down = true;
			break;
		}
	}

	void keyUp(KeyCode code, wchar_t character) {
		switch (code) {
		case Key_Left:
			left = false;
			break;
		case Key_Right:
			right = false;
			break;
		case Key_Up:
			up = false;
			break;
		case Key_Down:
			down = false;
			break;
		}
	}


	void init() {
		FileReader vs("shader.vert");
		FileReader fs("shader.frag");
		vertexShader = new Shader(vs.readAll(), vs.size(), VertexShader);
		fragmentShader = new Shader(fs.readAll(), fs.size(), FragmentShader);

		// This defines the structure of your Vertex Buffer
		VertexStructure structure;
		structure.add("pos", Float3VertexData);
		structure.add("tex", Float2VertexData);
		structure.add("nor", Float3VertexData);

		program = new Program;
		program->setVertexShader(vertexShader);
		program->setFragmentShader(fragmentShader);
		program->link(structure);

		tex = program->getTextureUnit("tex");
		pLocation = program->getConstantLocation("P");
		vLocation = program->getConstantLocation("V");
		mLocation = program->getConstantLocation("M");

		objects = new MeshObject*[101];
		for (int i = 0; i < 101; ++i) objects[i] = nullptr;

		MeshData* mesh = new MeshData("box.obj", structure);
		for (int y = 0; y < 10; ++y) {
			for (int x = 0; x < 10; ++x) {
				objects[y * 10 + x] = new MeshObject(mesh, new Texture("darmstadtmini.png", true), vec3((x - 5.0f) * 10, 0, (y - 5.0f) * 10));
			}
		}

		angle = 0.0f;

		Graphics::setRenderState(DepthTest, true);
		Graphics::setRenderState(DepthTestCompare, ZCompareLess);

		Graphics::setTextureAddressing(tex, Kore::U, Repeat);
		Graphics::setTextureAddressing(tex, Kore::V, Repeat);
	}
}

int kore(int argc, char** argv) {
	Kore::System::init("Exercise 11", width, height);
	
	init();

	Kore::System::setCallback(update);

	startTime = System::time();
	Kore::Mixer::init();
	Kore::Audio::init();
	//Kore::Mixer::play(new SoundStream("back.ogg", true));
	
	Keyboard::the()->KeyDown = keyDown;
	Keyboard::the()->KeyUp = keyUp;
	Mouse::the()->Move = mouseMove;
	Mouse::the()->Press = mousePress;
	Mouse::the()->Release = mouseRelease;

	streamMutex.Create();
	Kore::threadsInit();
	streamingThread = Kore::createAndRunThread(stream, nullptr);

	Kore::System::start();
	
	return 0;
}
