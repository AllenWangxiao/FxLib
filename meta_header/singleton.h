#ifndef SINGLETON_H
#define SINGLETON_H

#define DECLARE_SINGLETON(cls)\
private:\
    static cls* m_poInstance;\
public:\
    static bool CreateInstance()\
    {\
        if(0 == m_poInstance)\
            m_poInstance = new cls;\
        return m_poInstance != 0;\
    }\
    static cls* Instance(){ return m_poInstance; }\
    static void DestroyInstance()\
    {\
        if(m_poInstance != 0)\
        {\
            delete m_poInstance;\
            m_poInstance = 0;\
        }\
    }

#define IMPLEMENT_SINGLETON(cls) \
    cls* cls::m_poInstance = 0;

template<class T>
class TSingletion
{
public:
	virtual ~TSingletion() {}
	//static volatile T* Instance() { return (T*)(m_pInstance); }
	static T* Instance() { return (T*)(m_pInstance); }
	static bool CreateInstance()
	{
		if (!m_pInstance)
		{
			m_pInstance = new T();
			return m_pInstance != 0;
		}
		return false;
	}

	static bool DestroyInstance()
	{
		if (m_pInstance)
		{
			delete m_pInstance;
			m_pInstance = 0;
			return true;
		}
		return false;
	}

private:
	//static volatile T* m_pInstance;
	static T* m_pInstance;
protected:
	TSingletion() {}
};

template<class T>
T* TSingletion<T>::m_pInstance = 0;

#endif
