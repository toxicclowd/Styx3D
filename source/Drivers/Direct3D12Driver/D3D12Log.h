/****************************************************************************************/
/*  D3D12LOG.H                                                                          */
/*                                                                                      */
/*  Author: Styx3D Modernization                                                        */
/*  Description: Logging utility for DirectX 12 driver                                  */
/*                                                                                      */
/****************************************************************************************/
#ifndef D3D12LOG_H
#define D3D12LOG_H

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

class D3D12Log
{
private:
	static D3D12Log*	m_pInstance;
	FILE*				m_pFile;
	bool				m_bInitialized;

	D3D12Log() : m_pFile(nullptr), m_bInitialized(false)
	{
		Initialize();
	}

public:
	static D3D12Log* GetPtr()
	{
		if (!m_pInstance)
			m_pInstance = new D3D12Log();
		return m_pInstance;
	}

	bool Initialize()
	{
		if (m_bInitialized)
			return true;

		errno_t err = fopen_s(&m_pFile, "Direct3D12Driver.log", "w");
		if (err != 0 || !m_pFile)
		{
			m_pFile = nullptr;
			return false;
		}

		m_bInitialized = true;
		Printf("D3D12 Log initialized");
		return true;
	}

	void Shutdown()
	{
		if (m_pFile)
		{
			Printf("D3D12 Log shutdown");
			fclose(m_pFile);
			m_pFile = nullptr;
		}
		m_bInitialized = false;
	}

	void Printf(const char* format, ...)
	{
		if (!m_pFile)
			return;

		va_list args;
		va_start(args, format);

		char buffer[1024];
		vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);

		fprintf(m_pFile, "%s\n", buffer);
		fflush(m_pFile);

		// Also output to debug console
		OutputDebugStringA(buffer);
		OutputDebugStringA("\n");

		va_end(args);
	}

	~D3D12Log()
	{
		Shutdown();
	}
};

#endif // D3D12LOG_H
