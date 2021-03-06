// ExceptEvent.cpp: implementation of the CExceptEvent class.
//
//////////////////////////////////////////////////////////////////////

#include "ExceptEvent.h"
#include "UseDebugger.h"

#define  MAX_INSTRUCTION    15

static const unsigned char gs_BP = 0xCC;
static char gs_szCodeBuf[64];
static char gs_szOpcode[64];
static char gs_szASM[128];

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CExceptEvent::CExceptEvent()
{
    SYSTEM_INFO  sysInfo;
    GetSystemInfo(&sysInfo);
    m_dwPageSize = sysInfo.dwPageSize;

    m_szLastASM[0] = '\0';
}

CExceptEvent::~CExceptEvent()
{

}

/************************************************************************/
/* 
Function : Check whether hit the MemBP
Params   : dwAddr is the addr to be checked
           ppageBP contains the PageBP info where the dwAddr in
Process:
                                             */
/************************************************************************/
BOOL 
CExceptEvent::CheckHitMemBP(CBaseEvent *pEvent, DWORD dwAddr, tagPageBP *ppageBP)
{
    assert(ppageBP != NULL);
    DWORD dwOffset = dwAddr - ppageBP->dwPageAddr;
    BOOL bRet = FALSE;
    BOOL bTraced = FALSE;  //trace only once

    g_szBuf[0] = '\0';

    const char *pszASM;
    tagMemBPInPage *pmemBPInPage = NULL;
    list<tagMemBPInPage>::iterator it;
    for (it = ppageBP->lstMemBP.begin();
         it != ppageBP->lstMemBP.end();
         it++)
    {
        pmemBPInPage = &(*it);
        if (dwOffset >= pmemBPInPage->wOffset
            && dwOffset < pmemBPInPage->wOffset + pmemBPInPage->wSize)
        {
            //hit and if for trace, 
            if (pmemBPInPage->bTrace
                && !bTraced)
            {
                //need to log this instruction
                bTraced = TRUE;
                pszASM = this->GetOneASM(pEvent);
                if (0 == strcmp(pszASM, m_szLastASM))
                {
                    //to avoid repeat recording, like repxxx
                    continue;
                }

                strcpy(m_szLastASM, pszASM);
                pEvent->m_pUI->TraceLog(pszASM);  
                continue;
            }
            
            bRet = TRUE;
            _snprintf(g_szBuf, MAXBUF, "%sPage: %p Offset: %04X Size: %04X\r\n",
                                        g_szBuf,
                                        ppageBP->dwPageAddr,
                                        pmemBPInPage->wOffset,
                                        pmemBPInPage->wSize
                                        );   
        }
    }
    
    if (bRet)
    {
        pEvent->m_pUI->ShowInfo(g_szBuf);
    }
         
    return bRet;
}

/************************************************************************/
/* 
Function : 
Process  : take care of memory breakpoint
         1) whether memory breakpoints exists within the page  
         2) if yes, restore protect
                    set single step to change back                                                                 */
/************************************************************************/
DWORD
CExceptEvent::OnAccessViolation(CBaseEvent *pEvent)
{
    assert(pEvent != NULL);

    DWORD dwContinueStatus = DBG_EXCEPTION_NOT_HANDLED; 
    EXCEPTION_DEBUG_INFO exceptInfo = pEvent->m_debugEvent.u.Exception;
    EXCEPTION_RECORD exceptRecord = exceptInfo.ExceptionRecord;
    DWORD dwAddr = exceptRecord.ExceptionInformation[1];

    //whether exists memory BP
    tagPageBP *ppageBP = NULL;
    DWORD dwOldProtect;
    BOOL bRet = HasMemBP(pEvent, dwAddr, &ppageBP);
    if (bRet)
    {
        //now judge whether hit the MemBP
        bRet = CheckHitMemBP(pEvent, dwAddr, ppageBP);
        if (bRet)
        {
            _snprintf(g_szBuf, MAXBUF, "Hit MemBP %p %s***********\r\n\r\n",
                                        dwAddr,
                                        0 == exceptRecord.ExceptionInformation[0] ? "read" : "write"
                    );
            pEvent->m_pUI->ShowInfo(g_szBuf);
            DoShowRegs(pEvent);
            pEvent->m_bTalk = FALSE;/*TRUE*/;
        }

        //need to restore the protect, (and add PAGE_READWRITE)
        bRet = VirtualProtectEx(pEvent->m_hProcess,
                                (LPVOID)dwAddr,
                                MAX_INSTRUCTION,
                                ppageBP->dwOldProtect,
                                &dwOldProtect
                                );
        if (!bRet)
        {
            CUI::ShowErrorMessage();
            return DBG_CONTINUE;    //really?
        }

        //need to set single step to restore the protect
        m_bAccessVioTF = TRUE;
        m_dwAddr       = dwAddr;
        DoStepInto(pEvent/*, 1, argv, g_szBuf*/);
        return DBG_CONTINUE;
    }

    //just for curious
    DWORD dwFirstChance = pEvent->m_debugEvent.u.Exception.dwFirstChance;
    if (dwFirstChance)
    {
        pEvent->m_pUI->ShowInfo("\r\nAccessViolation First Chance*********\r\n");
        DoShowRegs(pEvent);
    }
    else
    {
        pEvent->m_pUI->ShowInfo("\r\nAccessViolation Second Chance*********\r\n");
        DoShowRegs(pEvent);
    }

    return dwContinueStatus;
}

DWORD
CExceptEvent::OnBreakPoint(CBaseEvent *pEvent)
{
    assert(pEvent != NULL);
    DWORD dwContinueStatus = DBG_EXCEPTION_NOT_HANDLED;
    
    //system breakpoint
    static BOOL bSysPoint = TRUE;
    if (bSysPoint)
    {
        bSysPoint = FALSE;
        pEvent->m_bTalk = TRUE;
        DoShowRegs(pEvent);

        return DBG_CONTINUE;
    }

    EXCEPTION_DEBUG_INFO exceptInfo = pEvent->m_debugEvent.u.Exception;
    EXCEPTION_RECORD exceptRecord = exceptInfo.ExceptionRecord;
    DWORD dwAddr = (DWORD)exceptRecord.ExceptionAddress;
    DWORD dwFirstChance = pEvent->m_debugEvent.u.Exception.dwFirstChance;

    //whether been set normalBP
    tagNormalBP *pNormalBP = NULL;
    BOOL bRet = HasNormalBP(pEvent, dwAddr, &pNormalBP);
    if (bRet)
    {
        assert(pNormalBP != NULL);

        //for NormalBP set on int 3
        if (pNormalBP->bDisabled)
        {
            goto NORMALBP_ON_INT3;
        }

        //whether NormalBP set on int 3
        if (gs_BP == pNormalBP->oldvalue)
        {
            //disabled for a while
            pNormalBP->bDisabled = TRUE;

            //and no need to restore the byte, return directly
            pEvent->m_Context.Eip--;
            pEvent->m_bTalk = TRUE;
            DoShowRegs(pEvent);
            return DBG_CONTINUE;
        }
    
        //now restore the code
        bRet = WriteProcessMemory(pEvent->m_hProcess,
                                (LPVOID)dwAddr,
                                (LPVOID)&pNormalBP->oldvalue,
                                sizeof(gs_BP),
                                NULL);
        if (!bRet)
        {
            CUI::ShowErrorMessage();
        }

        if (pNormalBP->bPerment)
        {
            //need to set single step
            m_bNormalBPTF = TRUE;
            m_dwAddr = dwAddr;
            DoStepInto(pEvent);
        }
        else
        {
            //should remove it
            m_mapAddr_NormBP.erase(dwAddr);
        }

        pEvent->m_Context.Eip--;
        pEvent->m_bTalk = TRUE;
        DoShowRegs(pEvent);

        return DBG_CONTINUE;
    }

NORMALBP_ON_INT3:
    //just for curious, and NormalBP set on int 3
    if (dwFirstChance)
    {
        pEvent->m_pUI->ShowInfo("\r\nBreakPoint First Chance*********\r\n");

        if (pNormalBP != NULL
            && pNormalBP->bDisabled)
        {
            //re-enable
            pNormalBP->bDisabled = FALSE;
        }
    }
    else
    {
        pEvent->m_pUI->ShowInfo("\r\nBreakPoint Second Chance*********\r\n");
        //dwContinueStatus = DBG_CONTINUE;  //can we ?
    }
    DoShowRegs(pEvent);
    
    return dwContinueStatus;
}

/************************************************************************/
/* 
Function : Check whether hit the Hardware Breakpoint
Params   : pEvent
Return   : TRUE if yes, FALSE otherwise 
Process  : check DR6
           1) whether B0~B3 set
           2) whether single step                                                                */
/************************************************************************/
BOOL
CExceptEvent::HasHitHWBP(CBaseEvent *pEvent)
{
    //whether DR6 B0 ~B3 set
    DWORD dwIndex = 0;
    dwIndex = (pEvent->m_Context.Dr6 & 0x0F);
    if (0 == dwIndex)
    {
        return FALSE;
    }

    //whether single step caused the "hit",
    //"the single step mode is the highest-priority debug exception,
    //when BS is set, any of the other debug status bit may also be set
    tagDR6 *pDR6 = (tagDR6 *)(&pEvent->m_Context.Dr6);
    if (pDR6->BS)
    {
        return FALSE;
    }

    //whether need to interact with user
    pEvent->m_bTalk = TRUE;
    
    //BO~B3 may be set more than one
    tagHWBP hwBP;
    hwBP.pDRAddr[0] = &pEvent->m_Context.Dr0;
    hwBP.pDRAddr[1] = &pEvent->m_Context.Dr1;
    hwBP.pDRAddr[2] = &pEvent->m_Context.Dr2;
    hwBP.pDRAddr[3] = &pEvent->m_Context.Dr3;

    DWORD dwDR7;
    DWORD dwLENRW;
    DWORD i;
    DWORD dwB03 = 0;        //B0~B3
    DWORD dwCheck = 1;   //check BX is set
    while (dwCheck != 16) //1,2,4,8
    {
        if (0 == (dwIndex & dwCheck))
        {
            dwB03++;
            dwCheck <<= 1;
            continue;
        }        
        
        dwDR7 = pEvent->m_Context.Dr7;
        dwLENRW = dwDR7 >> 16;
        for (i = 0 ; i < dwB03; i++)
        {
            dwLENRW >>= 4;
        }
        
        hwBP.dwAddr = *(hwBP.pDRAddr[dwB03]);
        hwBP.dwType = dwLENRW & 0x3;
        dwLENRW >>= 2;
        hwBP.dwLen = (dwLENRW & 0x03) + 1;
        dwLENRW >>= 2;
        
        //take care of execute
        if (HWBP_EXECUTE == hwBP.dwType)
        {
            hwBP.dwLen = 0;
        }
        
        sprintf(g_szBuf, "\r\nHWBP Hit: %p\t%d\t%s *****************\r\n",
                                    hwBP.dwAddr,
                                    hwBP.dwLen,
                                    (HWBP_EXECUTE == hwBP.dwType) ? STREXECUTE : 
                                    ((HWBP_WRITE == hwBP.dwType) ? STRWRITE: STRACCESS)
                                    );
        pEvent->m_pUI->ShowInfo(g_szBuf);
        
        //now disable the HWBP for a moment to skip, and re-enable within single step
        //only need for HWBP_EXECUTE
        //how about EFLAGS.RF ?
        //bhc dwIndex
        if (HWBP_EXECUTE == hwBP.dwType)
        {
            int argv[] = {0, 4};
            sprintf(g_szBuf, "bhc %d", dwB03);
            DoBHC(pEvent, 2, argv, g_szBuf);
            
            m_bHWBPTF = TRUE;
            m_dwAddr  = hwBP.dwAddr;
            DoStepInto(pEvent);
        }

        //remember to clear DR6
        pEvent->m_Context.Dr6 = 0;

        //if changing FS:[0], SEH Chain, then no need to interact with the user
        if (hwBP.dwAddr > 0x7F000000)
        {
            ((CUseDebugger *)pEvent)->DoShowSEH(NULL, NULL, NULL);            
            pEvent->m_bTalk = FALSE;
        }

        dwCheck <<= 1;
    }

    DoShowRegs(pEvent);
    return TRUE;

#if 0
    //here we only take care of one, not a good idea
    for (int i = 0; dwIndex != 1; i++)
    {
        dwIndex >>= 1;
    }

    dwIndex = i;
    
    tagHWBP hwBP;
    hwBP.pDRAddr[0] = &pEvent->m_Context.Dr0;
    hwBP.pDRAddr[1] = &pEvent->m_Context.Dr1;
    hwBP.pDRAddr[2] = &pEvent->m_Context.Dr2;
    hwBP.pDRAddr[3] = &pEvent->m_Context.Dr3;
    
    DWORD dwDR7 = pEvent->m_Context.Dr7;
    DWORD dwLENRW = dwDR7 >> 16;
    for (i = 0 ; i < dwIndex; i++)
    {
        dwLENRW >>= 4;
    }

    hwBP.dwAddr = *(hwBP.pDRAddr[dwIndex]);
    hwBP.dwType = dwLENRW & 0x3;
    dwLENRW >>= 2;
    hwBP.dwLen = (dwLENRW & 0x03) + 1;
    dwLENRW >>= 2;

    //take care of execute
    if (HWBP_EXECUTE == hwBP.dwType)
    {
        hwBP.dwLen = 0;
    }

    DoShowRegs(pEvent);
    sprintf(g_szBuf, "\r\nHWBP Hit: %p\t%d\t%s *****************\r\n",
                                hwBP.dwAddr,
                                hwBP.dwLen,
                                (HWBP_EXECUTE == hwBP.dwType) ? STREXECUTE : 
                                ((HWBP_WRITE == hwBP.dwType) ? STRWRITE: STRACCESS)
            );
    pEvent->m_pUI->ShowInfo(g_szBuf);

    //now disable the HWBP for a moment to skip, and re-enable within single step
    //only need for HWBP_EXECUTE
    //what about EFLAGS.RF ?
    //bhc dwIndex
    if (HWBP_EXECUTE == hwBP.dwType)
    {
        int argv[] = {0, 4};
        sprintf(g_szBuf, "bhc %d", dwIndex);
        DoBHC(pEvent, 2, argv, g_szBuf);
        
        m_bHWBPTF = TRUE;
        m_dwAddr  = hwBP.dwAddr;
        DoStepInto(pEvent);
    }
#endif
}

DWORD
CExceptEvent::OnSingleStep(CBaseEvent *pEvent)
{
    assert(pEvent != NULL);
    DWORD dwContinueStatus = DBG_EXCEPTION_NOT_HANDLED;
    DWORD dwFirstChance    = pEvent->m_debugEvent.u.Exception.dwFirstChance;

    if (pEvent->m_Context.Eip > 0x70000000)
    {
        int i = 0;
        //return DBG_CONTINUE;
    }

    //////////////////////////////////////////////////////////////////////////
    //re-enable the HWBP, but not guaranteed to the same index
    if (m_bHWBPTF)
    {
        m_bHWBPTF = FALSE;

        _snprintf(g_szBuf, MAXBUF, "re-enable HWBP at eip: %p\r\n",
                                   pEvent->m_Context.Eip);
        pEvent->m_pUI->ShowInfo(g_szBuf);

        //bh 00400000 e 0
        int argv[] = {0, 3, 0x0C, 0x0e};
        sprintf(g_szBuf, "bh %p e 0", m_dwAddr);
        DoBH(pEvent, 4, argv, g_szBuf);
        return DBG_CONTINUE;
    }

    //////////////////////////////////////////////////////////////////////////
    //check hardware
    if (HasHitHWBP(pEvent))
    {
        return DBG_CONTINUE;
    }
    
    //////////////////////////////////////////////////////////////////////////
    //AccessViolation
    tagPageBP *ppageBP = NULL;
    DWORD dwOldProtect;
    BOOL bRet;
    if (m_bAccessVioTF)
    {
        m_bAccessVioTF = FALSE;
        bRet = HasMemBP(pEvent, m_dwAddr, &ppageBP);
        if (bRet)
        {
            //need to restore the protect (PAGE_NOACCESS)
            bRet = VirtualProtectEx(pEvent->m_hProcess,
                                    (LPVOID)m_dwAddr,
                                    MAX_INSTRUCTION,
                                    ppageBP->dwNewProtect,
                                    &dwOldProtect
                                    );
            if (!bRet)
            {
                CUI::ShowErrorMessage();
                return DBG_CONTINUE;    //really?
            }
        }
        return DBG_CONTINUE;
    }

    //////////////////////////////////////////////////////////////////////////
    //NormalBP
    tagNormalBP *pNomalBP = NULL;
    if (m_bNormalBPTF)
    {
        m_bNormalBPTF = FALSE;

        bRet = HasNormalBP(pEvent, m_dwAddr, &pNomalBP);
        if (!bRet)
        {
            return DBG_CONTINUE;
        }

        //restore the code
        assert(pNomalBP->bPerment);
        bRet = WriteProcessMemory(pEvent->m_hProcess,
                                (LPVOID)m_dwAddr,
                                (LPVOID)&g_szBuf,
                                sizeof(gs_BP),
                                NULL);
        if (!bRet)
        {
            CUI::ShowErrorMessage();
        }

        if (pNomalBP->bPerment
            && pNomalBP->oldvalue != gs_BP  //for NormalBP set on int 3
            )
        {
            pEvent->m_bTalk = TRUE;
            DoShowRegs(pEvent);
        }

        return DBG_CONTINUE;
    }

    //////////////////////////////////////////////////////////////////////////
    //now for user input 't'
    if (pEvent->m_bUserTF)
    {
        pEvent->m_bUserTF = FALSE;
        pEvent->m_bTalk = TRUE;
        DoShowRegs(pEvent);
        return DBG_CONTINUE;
    }

    //////////////////////////////////////////////////////////////////////////
    //for Step Over
    if (pEvent->m_bStepOverTF)
    {
        pEvent->m_bStepOverTF = FALSE;
        pEvent->m_bTalk = TRUE;
        DoShowRegs(pEvent);
        return DBG_CONTINUE;
    }

    //////////////////////////////////////////////////////////////////////////
    //secondchance
    if (dwFirstChance)
    {
        pEvent->m_pUI->ShowInfo("\r\nSingleStep First Chance*********\r\n");
    }
    else
    {
        pEvent->m_pUI->ShowInfo("\r\nSingleStep Second Chance*********\r\n");
        //dwContinueStatus = DBG_CONTINUE;  //can we ?
    }
    DoShowRegs(pEvent);    

    return dwContinueStatus;
}

//////////////////////////////////////////////////////////////////////////
BOOL
CExceptEvent::DoStepOver(CBaseEvent *pEvent/*, int argc, int pargv[], const char *pszBuf*/)
{
    pEvent->m_bStepOverTF = TRUE;
    DWORD nCodeLen = 0;
    if (!IsCall(pEvent, &nCodeLen))
    {
        DoStepInto(pEvent);   
    }
    else
    {
        //bp addr 
        int argv[] = {0, 3};
        sprintf(g_szBuf, "bp %p", pEvent->m_Context.Eip + nCodeLen); 
        
        pEvent->m_bTmpBP = TRUE;
        DoBP(pEvent, 2, argv, g_szBuf);
    }

    return TRUE;  
}

BOOL
CExceptEvent::DoStepInto(CBaseEvent *pEvent/*, int argc, int pargv[], const char *pszBuf*/)
{
    pEvent->m_Context.EFlags |= 0x100;
    return TRUE;  
}

BOOL
CExceptEvent::DoGo(CBaseEvent *pEvent, int argc, int pargv[], const char *pszBuf)
{
    //g  or g addr
    assert(pEvent != NULL);
    assert(pszBuf != NULL);

    //g
    if (1 == argc)
    {
        return TRUE;
    }
    
    //g addr   now need to set a tmp NormalBreakPoint
    pEvent->m_bTmpBP = TRUE;
    return DoBP(pEvent, argc, pargv, pszBuf);
}

/************************************************************************/
/* 
Function : judge whether the page where the addr in is valid
Params   : dwAddr is the specified address
Return   : TRUE is valid, FALSE otherwise                               */
/************************************************************************/
BOOL 
CExceptEvent::IsPageValid(CBaseEvent *pEvent, DWORD dwAddr)
{
    assert(pEvent != NULL);

    MEMORY_BASIC_INFORMATION memInfo;
    VirtualQueryEx(pEvent->m_hProcess,
                (LPVOID)dwAddr,
                &memInfo,
                sizeof(MEMORY_BASIC_INFORMATION)
                );
    
    if (memInfo.State != MEM_COMMIT)
    {
        return FALSE;
    }

    return TRUE;
}

/************************************************************************/
/* 
Function : judge whether the page where the addr in exists memory breakpoint
Params   : dwAddr is the specified address
           [OUT] pPageBP used to receive the pageBP info (if exists)
Return   : TRUE if exists, FALSE otherwise                                                                     */
/************************************************************************/
BOOL
CExceptEvent::HasMemBP(CBaseEvent *pEvent, DWORD dwAddr, tagPageBP **ppPageBP)
{
    assert(pEvent != NULL); 
    assert(ppPageBP != NULL);
    *ppPageBP = NULL;

    DWORD dwPageAddr = (dwAddr / m_dwPageSize) * m_dwPageSize;
    map<DWORD, tagPageBP>::iterator it;
    it = m_mapPage_PageBP.find(dwPageAddr);
    if (it != m_mapPage_PageBP.end())
    {
        *ppPageBP = &it->second;
        return TRUE;
    }

    return FALSE;
}

/************************************************************************/
/* 
Function : judge whether the addr HAS NormalBP RECORD
           BUT IT'S THE USER'S DUTY to dertermine 
           1) whether it's tmp or permanent, 
           //2) if permanent,  whether, enabled or disabled.
Params   : dwAddr is the specified address
            [OUT] ppNormalBP used to receive the normalBP info (if exists)
Return   : TRUE if already been recorded.
            FALSE otherwise              
Alert    : Again, IT'S THE USER'S DUTY to dertermine 
            1) whether it's tmp or permanent, 
            //2) if permanent,  whether, enabled or disabled*/
/************************************************************************/
BOOL
CExceptEvent::HasNormalBP(CBaseEvent *pEvent, DWORD dwAddr, tagNormalBP **ppNormalBP)
{
    assert(pEvent != NULL);
    assert(ppNormalBP != NULL);
    *ppNormalBP = NULL;

    map<DWORD, tagNormalBP>::iterator it;
    it = m_mapAddr_NormBP.find(dwAddr);
    if (it != m_mapAddr_NormBP.end())
    {
        *ppNormalBP = &it->second;
        return TRUE;
    }

    return FALSE;
}

/************************************************************************/
/* 
Function : set breakpoint at the specified address
Process  :
         1) whether already been set
         2) whether the page valid 
         3) consider the page may be changed by Memory Point
                                                                     */
/************************************************************************/
BOOL
CExceptEvent::DoBP(CBaseEvent *pEvent, int argc, int pargv[], const char *pszBuf)
{  
    //bp addr
    assert(2 == argc);
    assert(pEvent != NULL);

    BOOL bRet;
    DWORD dwAddr = strtoul(&pszBuf[pargv[1]], NULL, 16);
    assert((dwAddr != 0) && (dwAddr != ULONG_MAX));

    //whether has the record, (but not sure tmp or permanent, //enabled or disabled)
    tagNormalBP *pNormalBP = NULL;
    bRet = HasNormalBP(pEvent, dwAddr, &pNormalBP);
    if (bRet)
    {
        assert(pNormalBP != NULL);

        //if to set tmp
        if (pEvent->m_bTmpBP)
        {
            if (pNormalBP->bTmp)
            {
                //
            }
            else if (pNormalBP->bPerment)
            {
                pNormalBP->bTmp = TRUE;
            }
            else
            {
                //both not tmp and permanent, no possible
                assert(FALSE);
            }
            pEvent->m_bTmpBP = FALSE;
        }
        else    
        {
            if (pNormalBP->bTmp)
            {
                pNormalBP->bPerment = TRUE;
            }
            else if (pNormalBP->bPerment)
            {
                //
            }
            else
            {
                //not possible
                assert(FALSE);
            }
        }

        //for setting NormalBP on int 3 , not a good idea
        pNormalBP->bDisabled = FALSE;
        return TRUE;
    }

    //////////////////////////////////////////////////////////////////////////
    //Now has no record about the normalbp

    //whether the page valid
    bRet = IsPageValid(pEvent, dwAddr);
    if (!bRet)
    {
        return FALSE;
    }

    //whether memory breakpoint exists within the page
    tagPageBP *ppageBP = NULL;
    DWORD dwOldProtect;
    bRet = HasMemBP(pEvent, dwAddr, &ppageBP);

    //now save the NormalBP
    tagNormalBP normalBP = {0};
    bRet = ReadBuf(pEvent,
                   pEvent->m_hProcess,
                   (LPVOID)dwAddr,
                   (LPVOID)&normalBP.oldvalue,
                   sizeof(normalBP.oldvalue)
                  );
    if (!bRet)
    {
        return FALSE;
    }

    //bkz, we've re-enable the MemBP within ReadBuf, not so good
    if (ppageBP != NULL)
    {
        bRet = VirtualProtectEx(pEvent->m_hProcess,
                            (LPVOID)dwAddr,
                            MAX_INSTRUCTION,
                            ppageBP->dwOldProtect,
                            &dwOldProtect
                            );
    }

    bRet = WriteProcessMemory(pEvent->m_hProcess,
                        (LPVOID)dwAddr,
                        (LPVOID)&gs_BP,
                        sizeof(gs_BP),
                        NULL
                        );
    if (!bRet)
    {
        CUI::ShowErrorMessage();
        return FALSE;
    }

    //now save the NormalBP
    if (pEvent->m_bTmpBP)
    {
        normalBP.bTmp = TRUE;
        pEvent->m_bTmpBP = FALSE;
    }
    else
    {
        normalBP.bPerment = TRUE;
    }
    normalBP.bDisabled = FALSE;   //for setting NormalBP on int 3
    m_mapAddr_NormBP[dwAddr] = normalBP;

    //restore the protect
    if (ppageBP != NULL)
    {
        bRet = VirtualProtectEx(pEvent->m_hProcess,
                            (LPVOID)dwAddr,
                            MAX_INSTRUCTION,
                            ppageBP->dwNewProtect,
                            &dwOldProtect
                            );
    }

    return TRUE;
}

BOOL
CExceptEvent::DoBPL(CBaseEvent *pEvent/*, int argc, int pargv[], const char *pszBuf*/)
{
    assert(pEvent != NULL);

    sprintf(g_szBuf, "----------------普通断点列表----------------\r\n"
                     "序号\t地址\r\n");

    tagNormalBP *pNormalBP = NULL;  
    int i = 0;
    map<DWORD, tagNormalBP>::iterator it;
    for (it = m_mapAddr_NormBP.begin(); 
        it != m_mapAddr_NormBP.end();
        it++, i++)
    {
        pNormalBP = &it->second;
        if (pNormalBP->bPerment)
        {
            _snprintf(g_szBuf, MAXBUF, "%s%d\t%p\r\n",
                                        g_szBuf,
                                        i,
                                        it->first
                                        );
        }
    }

    pEvent->m_pUI->ShowInfo(g_szBuf);    
    return TRUE;
}

/************************************************************************/
/* 
Function : remove the specified normal breakpoint  
Params   :                     
           pszBuf[pargv[0]] = "bpc"
           pszBuf[pargv[1]] = id
/************************************************************************/
BOOL
CExceptEvent::DoBPC(CBaseEvent *pEvent, int argc, int pargv[], const char *pszBuf)
{
    //bpc id
    assert(pEvent != NULL);
    assert(pszBuf != NULL);
    assert(2 == argc);
    assert(isdigit(pszBuf[pargv[1]]));

    //can be more beautiful
    DWORD dwIndex = strtoul(&pszBuf[pargv[1]], NULL, 10);
    assert(dwIndex != ULONG_MAX);

    //not a good idea to delete by id within
    tagNormalBP *pNormalBP = NULL;  
    DWORD i = 0;
    map<DWORD, tagNormalBP>::iterator it;
    for (it = m_mapAddr_NormBP.begin(); 
        it != m_mapAddr_NormBP.end();
        it++, i++)
    {
        if (i == dwIndex )
        {
            m_mapAddr_NormBP.erase(it);
            return TRUE;
        }      
    }

    return FALSE;
}

/************************************************************************/
/* 
Function : check Memory BreakPoint validity  
Params   : pEvents  contains hProcess
           pMemBP contains the membp info

Return   : TRUE is valid, and set some info
           Otherwise, FALSE 

Process :  
        1) if already exist
        2) stop if not MEM_COMMIT
        3) whether already been set (memory breakpoint already exists)
        4) whether necessary to set (like no need to set WRITE if it is READONLY
        */
/************************************************************************/
BOOL 
CExceptEvent::CheckBMValidity(CBaseEvent *pEvent, 
                              tagMemBP *pMemBP)
{
    assert(pEvent != NULL);

    //how many pages may be involed
    DWORD nPages = (pMemBP->dwAddr + pMemBP->dwSize) / m_dwPageSize - pMemBP->dwAddr / m_dwPageSize;
    if (0 == nPages)
    {
        nPages = 1;
    }

    //check these memory state 
    MEMORY_BASIC_INFORMATION memInfo;
    tagMemBPInPage memBPInPage;         //断点在分页内信息
    map<DWORD, tagPageBP>::iterator it;
    map<DWORD, tagPageBP>::iterator itend = m_mapPage_PageBP.end();
    list<tagMemBP>::iterator itMemBP;
    DWORD  dwPageAddr = (pMemBP->dwAddr / m_dwPageSize) * m_dwPageSize;
    DWORD dwOldProtect;
    DWORD dwRealSize = 0;
    BOOL bRet;

    //if already exist
    itMemBP = find(m_lstMemBP.begin(), m_lstMemBP.end(), *pMemBP);
    if (itMemBP != m_lstMemBP.end())
    {
        return FALSE;
    }

    for (DWORD i = 0; i < nPages; i++)
    {
        VirtualQueryEx(pEvent->m_hProcess,
                        (LPVOID)dwPageAddr,
                        &memInfo,
                        sizeof(MEMORY_BASIC_INFORMATION)
                        );

        //not deal with MEM_FREE, MEM_RESERVE
        if (memInfo.State != MEM_COMMIT)
        {
            pEvent->m_pUI->ShowInfo("not MEM_COMMIT\r\n");
            break;
        }

        //if protect already set
        if (PAGE_NOACCESS == memInfo.Protect)
        {
            it = m_mapPage_PageBP.find(dwPageAddr);
            if (it == itend)
            {
                dwPageAddr += m_dwPageSize;
                continue;
            }
            memInfo.Protect = (*it).second.dwOldProtect;
        }

        //if no need to set
        if ((MEMBP_WRITE == pMemBP->dwType)
            && (PAGE_READONLY == pMemBP->dwType
                || PAGE_EXECUTE == pMemBP->dwType
                || PAGE_EXECUTE_READ == pMemBP->dwType)
                //others?
                )
        {
            dwPageAddr += m_dwPageSize;
            continue;
        }
       
        //can be more beautiful
        if (i > 0
            && i < nPages - 1)
        {
            memBPInPage.wOffset = 0;
            memBPInPage.wSize   = m_dwPageSize;
        }
        else if (0 == i)
        {
            memBPInPage.wOffset = pMemBP->dwAddr - dwPageAddr;
            memBPInPage.wSize   = min(pMemBP->dwSize, m_dwPageSize - memBPInPage.wOffset);
        }
        else    //i = nPages - 1
        {
            memBPInPage.wOffset = 0;
            memBPInPage.wSize   = pMemBP->dwAddr + pMemBP->dwSize - dwPageAddr;
        }
        memBPInPage.bTrace = pMemBP->bTrace;

        //if size is zero, all done
        if (0 == memBPInPage.wSize)
        {
            break;
        }

        m_mapPage_PageBP[dwPageAddr].dwPageAddr   = dwPageAddr;
        m_mapPage_PageBP[dwPageAddr].dwOldProtect = memInfo.Protect;
        m_mapPage_PageBP[dwPageAddr].dwNewProtect = PAGE_NOACCESS;
        m_mapPage_PageBP[dwPageAddr].lstMemBP.remove(memBPInPage);    //to avoid already exists
        m_mapPage_PageBP[dwPageAddr].lstMemBP.push_back(memBPInPage);

        //now change the protect
        bRet = VirtualProtectEx(pEvent->m_hProcess,
                            (LPVOID)dwPageAddr,
                            MAX_INSTRUCTION,
                            PAGE_NOACCESS,
                            &dwOldProtect
                             );
        if (!bRet)
        {
            CUI::ShowErrorMessage();
        }

        dwPageAddr += m_dwPageSize;        
    } 

    //valid, just take it
    if (i != 0)
    {
        m_lstMemBP.push_back(*pMemBP);
    }

    return TRUE;
}

/************************************************************************/
/* 
Function : set memory breakpoint   
Params   :  bm addr a|w len 
            addr为断点起始值，a|w分别表示访问类型和写入类型，len表示断点的长度
            bTrace used to indicate whether this used for trace instruction.
Process: 
1)断点合法性检查（分页是否有效，断点属性与分页属性，重复性设置），
2)新属性的设置，
3)分页断点的信息更新 

内存断点与分页关系的维护： 
1) 考虑模仿重定位表来维护分页内断点信息。
2) 用户输入的内存断点信息，独立的保存 （仅为显示所用）
3) 相关的分页中同时也维护涉及到的断点信息 （触发时的处理）
                                                                 */
/************************************************************************/
BOOL
CExceptEvent::DoBM(CBaseEvent *pEvent, 
                   int argc, 
                   int pargv[], 
                   const char *pszBuf,
                   BOOL bTrace
                   )
{ 
    //bm addr a|w len
    assert(4 == argc);
    assert(pEvent != NULL);

    if (!bTrace)
    {
        int i = 0;
    }

    DWORD dwAddr = strtoul(&pszBuf[pargv[1]], NULL, 16);
    assert((dwAddr != 0) && (dwAddr != ULONG_MAX));

    char  bpType  = pszBuf[pargv[2]];
    DWORD dwSize = strtoul(&pszBuf[pargv[3]], NULL, 10);
    assert((dwSize != 0) && (dwSize != ULONG_MAX));
    assert(('a' == bpType) || ('w' == bpType));

    //check address validity, 
    tagMemBP       memBP;               //独立内存断点
    memBP.dwAddr = dwAddr;
    memBP.dwSize = dwSize;
    memBP.dwType = ((bpType == 'a') ? MEMBP_ACCESS : MEMBP_WRITE);
    memBP.bTrace = bTrace;
    CheckBMValidity(pEvent, 
                    &memBP
                    );

    return TRUE;
}

BOOL
CExceptEvent::DoBML(CBaseEvent *pEvent, int argc, int pargv[], const char *pszBuf)
{  
    assert(pEvent != NULL);
    sprintf(g_szBuf, "----------------内存断点列表----------------\r\n"
                     "序号\t地址\t\t长度\t\t类型\r\n");

    list<tagMemBP>::iterator it;
    tagMemBP memBP;
    int i = 0;
    for (it = m_lstMemBP.begin(); 
         it != m_lstMemBP.end(); 
         it++, i++)
    {
        memBP = *it;
        _snprintf(g_szBuf, MAXBUF, "%s%d\t%p\t%p\t%s\r\n",
                                    g_szBuf,
                                    i,
                                    memBP.dwAddr, 
                                    memBP.dwSize,
                                    MEMBP_ACCESS == memBP.dwType ? "访问" : "写"
                                    );
    }
    pEvent->m_pUI->ShowInfo(g_szBuf);

    return TRUE;
}

BOOL 
CExceptEvent::DoBMPL(CBaseEvent *pEvent, int argc, int pargv[], const char *pszBuf)
{
    assert(pEvent != NULL);
    sprintf(g_szBuf, "----------------分页断点列表----------------\r\n");

    tagPageBP pageBP;
    tagMemBPInPage memBPInPage;
    map<DWORD, tagPageBP>::iterator it;
    list<tagMemBPInPage>::iterator itBPInPage;
    for (it = m_mapPage_PageBP.begin();
         it != m_mapPage_PageBP.end();
         it++)
    {
        pageBP = (*it).second;
        
        _snprintf(g_szBuf, MAXBUF, "%s分页地址\t旧属性\t\t新属性\r\n"
                                   "%p\t%p\t%p\r\n"
                                   "\t偏移\t长度\r\n",
                                    g_szBuf,
                                    pageBP.dwPageAddr,
                                    pageBP.dwOldProtect,
                                    pageBP.dwNewProtect);
        for (itBPInPage = pageBP.lstMemBP.begin();
             itBPInPage != pageBP.lstMemBP.end();
             itBPInPage++)
        {
            memBPInPage = *itBPInPage;
            _snprintf(g_szBuf, MAXBUF, "%s\t%04X\t%04X\r\n",
                                        g_szBuf,
                                        memBPInPage.wOffset,
                                        memBPInPage.wSize);
        }
    }

    pEvent->m_pUI->ShowInfo(g_szBuf);
    return TRUE;
}

/************************************************************************/
/* 
Function : judge whether has other memBP within the page
Params   : dwPageAddr indicate the page
           ppPageBP used to receive the PageBP info
           pnTotal used to receive the count of MemBPInPage
Return   : TRUE if has other memBPS, Otherwise FALSE                                                                    */
/************************************************************************/
BOOL
CExceptEvent::HasOtherMemBP(CBaseEvent *pEvent, 
                            DWORD dwPageAddr, 
                            tagPageBP **ppPageBP,
                            DWORD *pnTotal)
{
    assert(ppPageBP != NULL);
    *ppPageBP = &m_mapPage_PageBP[dwPageAddr];

    list<tagMemBPInPage> &lstmemBP = m_mapPage_PageBP[dwPageAddr].lstMemBP;
    list<tagMemBPInPage>::iterator it;
    DWORD i = 0;
    for (it = lstmemBP.begin();
        it != lstmemBP.end();
        it++, i++)
    {
        //nothing
    }

    *pnTotal = i;
    if (i > 1)
    {
        return TRUE;
    }

    return FALSE;
}

/************************************************************************/
/* 
Function : Remove the specified memory breakpoint
Params   : bmc id
           id is the index shown in bml

Process  :
                               */
/************************************************************************/
BOOL
CExceptEvent::DoBMC(CBaseEvent *pEvent, int argc, int pargv[], const char *pszBuf)
{
    //bmc id
    assert(pEvent != NULL);
    assert(pszBuf != NULL);
    assert(2 == argc);
    assert(isdigit(pszBuf[pargv[1]]));

    DWORD i = 0;
    DWORD j = strtoul(&pszBuf[pargv[1]], NULL, 10);  //not a good idea
    DWORD dwAddr = 0;
    DWORD dwSize = 0;
    DWORD dwType = 0;

    //get dwAddr, dwSize, dwType
    list<tagMemBP>::iterator itMemBP;
    for (itMemBP = m_lstMemBP.begin();
        itMemBP != m_lstMemBP.end();
        itMemBP++, i++)
    {
        if (j == i)
        {
            dwAddr = (*itMemBP).dwAddr;
            dwSize = (*itMemBP).dwSize;
            dwType = (*itMemBP).dwType;
            
            //remove from MemBP
            m_lstMemBP.remove(*itMemBP);
            break;
        }
    }

    //if no match
    if (0 == dwAddr)
    {
        return FALSE;
    }

    //how many pages may be involed
    DWORD nPages = (dwAddr + dwSize) / m_dwPageSize - dwAddr / m_dwPageSize;
    if (0 == nPages)
    {
        nPages = 1;
    }
    
    //check these pages state 
    MEMORY_BASIC_INFORMATION memInfo;
    tagMemBPInPage memBPInPage;         //断点在分页内信息
    tagPageBP      *ppageBP = NULL;
    map<DWORD, tagPageBP>::iterator it;
    map<DWORD, tagPageBP>::iterator itend = m_mapPage_PageBP.end();
    DWORD  dwPageAddr = (dwAddr / m_dwPageSize) * m_dwPageSize;
    DWORD dwOldProtect;
    BOOL bRet;

    //whether this page contains other membp
    for (i = 0; i < nPages; i++)
    {       
        VirtualQueryEx(pEvent->m_hProcess,
                        (LPVOID)dwPageAddr,
                        &memInfo,
                        sizeof(MEMORY_BASIC_INFORMATION)
                        );
        
        //not deal with MEM_FREE, MEM_RESERVE
        if (memInfo.State != MEM_COMMIT)
        {
            pEvent->m_pUI->ShowInfo("not MEM_COMMIT\r\n");
            break;
        }

        //if protect already not set
#if 0
        if (PAGE_NOACCESS != memInfo.Protect)
        {
            dwPageAddr += m_dwPageSize;
            continue;
        }
#endif
        
        //if no need to set
        if ((MEMBP_WRITE == dwType)
            && (PAGE_READONLY == dwType
            || PAGE_EXECUTE == dwType
            || PAGE_EXECUTE_READ == dwType)
            //others?
            )
        {
            dwPageAddr += m_dwPageSize;
            continue;
        }
        
        //can be more beautiful
        if (i > 0
            && i < nPages - 1)
        {
            memBPInPage.wOffset = 0;
            memBPInPage.wSize   = m_dwPageSize;
        }
        else if (0 == i)
        {
            memBPInPage.wOffset = dwAddr - dwPageAddr;
            memBPInPage.wSize   = min(dwSize, m_dwPageSize - memBPInPage.wOffset);
        }
        else    //i = nPages - 1
        {
            memBPInPage.wOffset = 0;
            memBPInPage.wSize   = dwAddr + dwSize - dwPageAddr;
        }

        //if has no other memBP within the page, now can restore the protect
        DWORD dwTotal = 0;
        if (!HasOtherMemBP(pEvent, dwPageAddr, &ppageBP, &dwTotal))
        {
            bRet = VirtualProtectEx(pEvent->m_hProcess,
                                (LPVOID)dwPageAddr,
                                MAX_INSTRUCTION,
                                ppageBP->dwOldProtect,
                                &dwOldProtect
                                );
            if (!bRet)
            {
                CUI::ShowErrorMessage();
            }
        }
        
        //remove from PageBP info
        m_mapPage_PageBP[dwPageAddr].lstMemBP.remove(memBPInPage);

        //if no others, then remove from m_mapPage_PageBP
        if (1 == dwTotal)
        {
            m_mapPage_PageBP.erase(dwPageAddr);
        }
                
        dwPageAddr += m_dwPageSize;        
    } 

    return TRUE;
}

/************************************************************************/
/* 
Function : Set Hardware Break Point for the specified addr and len 
Params   :  pHWBP contains the HWBP info by user

Process  :
          0) whether the addr valid
          1) whether DR0 ~ DR3 available
          2) fix the align 
          */
/************************************************************************/
BOOL
CExceptEvent::SetHWBP(CBaseEvent *pEvent, tagHWBP *pHWBP)
{
    assert(pEvent != NULL);
    assert(pHWBP != NULL);

    //whether the page valid
    BOOL bRet = IsPageValid(pEvent, pHWBP->dwAddr);
    if (!bRet)
    {
        return FALSE;
    }

    //fix align
    if (0x01 == (pHWBP->dwAddr & 0x01))
    {
        pHWBP->dwLen = 1;
    }
    else if ((0x2 == (pHWBP->dwAddr & 0x2))
            && (0x4 == pHWBP->dwLen)
            )
    {
        pHWBP->dwLen = 2;
    }

    //and fix bh addr e 0
    DWORD dwLen = pHWBP->dwLen - 1;  //00 ->1byte, 01 -> 2byte, 11 -> 3byte
    if (HWBP_EXECUTE == pHWBP->dwType)
    {
        pHWBP->dwLen = 0;
        dwLen = 0;
    }

    //
    tagDR7 *pdr7 = (tagDR7 *)(&pEvent->m_Context.Dr7);
    pHWBP->RW[0] = pdr7->RW0;
    pHWBP->RW[1] = pdr7->RW1;
    pHWBP->RW[2] = pdr7->RW2;
    pHWBP->RW[3] = pdr7->RW3;

    pEvent->m_Context.Dr7 |= DR7INIT;
    DWORD dwDR7 = pEvent->m_Context.Dr7;
    DWORD dwCheck = 0x03;
    DWORD dwSet   = 0x01;
    DWORD dwLENRW = (((dwLen << 2) | pHWBP->dwType) << 16);
    int i = 0;
    for ( ; i < 4; i++)
    {
        //if both GX, LX is zero, then DRX is available
        if (0 == (dwDR7 & dwCheck))
        {
            *(pHWBP->pDRAddr[i])  = pHWBP->dwAddr;                  //DR0 = dwAddr   
            pEvent->m_Context.Dr7 |= dwSet;                          //pdr7->GL0 = 1;
            pEvent->m_Context.Dr7 |= dwLENRW;
            break;
        }

        //if the same addr and type
        if ( (*(pHWBP->pDRAddr[i]) ==pHWBP->dwAddr)
            && pHWBP->RW[i] == pHWBP->dwType)
        {
            //just keep same, nothing changed
            return FALSE;
        }

        dwCheck <<= 2;
        dwSet   <<= 2;
        dwLENRW <<= 4;
    }

    //no availabe
    if (4 == i)
    {
        pEvent->m_pUI->ShowInfo("No DRX available\r\n");
        return FALSE;
    }

    return TRUE;

#if 0
    //find the available DR0~DR3, can be more beautiful
    DWORD *pDRX = NULL;
    int nFree = -1;
    tagDR7 *pdr7 = (tagDR7 *)(&pEvent->m_Context.Dr7);
    if (0 == pdr7->GL0)
    {
        nFree = 0;
        pDRX  = &pEvent->m_Context.Dr0;
        pdr7->GL0 = 1;
        pdr7->LEN0 = dwLen;
        pdr7->RW0  = pHWBP->dwType;
    }
    else if (0 == pdr7->GL1)
    {
        nFree = 1;
        pDRX  = &pEvent->m_Context.Dr1;
        pdr7->GL1 = 1;
        pdr7->LEN1 = dwLen;
        pdr7->RW1  = pHWBP->dwType;
    }
    else if (0 == pdr7->GL2)
    {
        nFree = 2;
        pDRX  = &pEvent->m_Context.Dr2;
        pdr7->GL2 = 1;
        pdr7->LEN2 = dwLen;
        pdr7->RW2  = pHWBP->dwType;
    }
    else if (0 == pdr7->GL3)
    {
        nFree = 3;
        pDRX  = &pEvent->m_Context.Dr3;
        pdr7->GL3 = 1;
        pdr7->LEN3 = dwLen;
        pdr7->RW3  = pHWBP->dwType;
    }
    
    if (-1 == nFree)
    {
        return FALSE;
    }

    return TRUE;
#endif 
}

BOOL 
CExceptEvent::DoBH(CBaseEvent *pEvent, int argc, int pargv[], const char *pszBuf)
{
    //bh addr e|w|a 1|2|4  (specially, we always set bh addr e 0) !!!
    assert(pEvent != NULL);
    assert(pszBuf != NULL);
    assert(4 == argc);
   
    DWORD dwAddr = strtoul(&pszBuf[pargv[1]], NULL, 16);
    assert((dwAddr != 0) && (dwAddr != ULONG_MAX));

    char chType  = pszBuf[pargv[2]];
    assert('e' == chType || 'w' == chType || 'a' == chType);
    DWORD dwType = ('e' == chType) ? HWBP_EXECUTE : 
                    (('w' == chType) ? HWBP_WRITE : HWBP_ACCESS );

    char chLen   = pszBuf[pargv[3]];
    assert('0' == chLen || '1' == chLen || '2' == chLen || '4' == chLen);
    DWORD dwLen = strtoul(&chLen, NULL, 10);

    //can be more beautiful, constructor
    tagHWBP hwBP;
    hwBP.dwAddr = dwAddr;
    hwBP.dwType = dwType;
    hwBP.dwLen  = dwLen;
    hwBP.pDRAddr[0] = &pEvent->m_Context.Dr0;
    hwBP.pDRAddr[1] = &pEvent->m_Context.Dr1;
    hwBP.pDRAddr[2] = &pEvent->m_Context.Dr2;
    hwBP.pDRAddr[3] = &pEvent->m_Context.Dr3;

    SetHWBP(pEvent, &hwBP);

    return TRUE;
}

BOOL 
CExceptEvent::DoBHL(CBaseEvent *pEvent/*, int argc, int pargv[], const char *pszBuf*/)
{
    tagDR7 *pdr7 = (tagDR7 *)(&pEvent->m_Context.Dr7);
    DWORD dwDR7 = pEvent->m_Context.Dr7;
    DWORD dwCheck = 0x03;
    DWORD dwLENRW = dwDR7 >> 16;
    tagHWBP hwBP;
    hwBP.pDRAddr[0] = &pEvent->m_Context.Dr0;   //can be more beautiful
    hwBP.pDRAddr[1] = &pEvent->m_Context.Dr1;
    hwBP.pDRAddr[2] = &pEvent->m_Context.Dr2;
    hwBP.pDRAddr[3] = &pEvent->m_Context.Dr3;

    sprintf(g_szBuf, "----------------硬件断点列表----------------\r\n"
                     "序号\t地址\t\t长度\t类型\r\n");
    int i = 0;
    for ( ; i < 4; i++)
    {
        //if both GX, LX is zero, then DRX is not set
        if (0 == (dwDR7 & dwCheck))
        {
            dwCheck <<= 2;
            dwLENRW >>= 4;
            continue;
        }
       
        dwCheck <<= 2;

        hwBP.dwAddr = *(hwBP.pDRAddr[i]);
        hwBP.dwType = dwLENRW & 0x3;
        dwLENRW >>= 2;
        hwBP.dwLen = (dwLENRW & 0x03) + 1;
        dwLENRW >>= 2;

        //take care of execute
        if (HWBP_EXECUTE == hwBP.dwType)
        {
            hwBP.dwLen = 0;
        }

        _snprintf(g_szBuf, MAXBUF, "%s%d\t%p\t%d\t%s\r\n",
                                    g_szBuf,
                                    i,
                                    hwBP.dwAddr,
                                    hwBP.dwLen,
                                    (HWBP_EXECUTE == hwBP.dwType) ? STREXECUTE : 
                                    ((HWBP_WRITE == hwBP.dwType) ? STRWRITE: STRACCESS)
                                    );
    }

    pEvent->m_pUI->ShowInfo(g_szBuf);

    return TRUE;
}  

/************************************************************************/
/* 
Function : remove the specified HWBP                                   */
/************************************************************************/
BOOL 
CExceptEvent::DoBHC(CBaseEvent *pEvent, int argc, int pargv[], const char *pszBuf)
{
    //bhc id
    assert(pEvent != NULL);
    assert(pszBuf != NULL);
    assert(isdigit(pszBuf[pargv[1]]));

    DWORD dwIndex = strtoul(&pszBuf[pargv[1]], NULL, 10);
    assert(dwIndex < 4);

    DWORD dwSet = 0x3;
    for (DWORD i = 0; i < dwIndex; i++)
    {
        dwSet <<= 2;
    }

    pEvent->m_Context.Dr7 &= (~dwSet);
    
    return TRUE;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//Registers related
void 
CExceptEvent::DoShowRegs(CBaseEvent *pEvent)
{
    assert(pEvent != NULL);

    //to avoid frequently asking TIB
    static DWORD dwFS = pEvent->m_Context.SegFs;
    static DWORD dwTIB = 0;
    if (dwFS != pEvent->m_dwFS)
    {
        dwFS = pEvent->m_dwFS;
        dwTIB = GetTIB(pEvent);
    }

    tagEFlags eflg = *(tagEFlags *)&pEvent->m_Context.EFlags;
    _snprintf(g_szBuf, MAXBUF, "EAX=%08X ECX=%08X EDX=%08X EBX=%08X\r\n"
                                "ESP=%08X EBP=%08X ESI=%08X EDI=%08X\r\n"
                                "EIP=%08X CS=%04X DS=%04X ES=%04X SS=%04X FS=%04X [%p]\r\n"
                                "OF=%1X DF=%1X IF=%1X TF=%1X SF=%1X ZF=%1X AF=%1X PF=%1X CF=%1X\r\n",
                                pEvent->m_Context.Eax, pEvent->m_Context.Ecx, pEvent->m_Context.Edx, pEvent->m_Context.Ebx,
                                pEvent->m_Context.Esp, pEvent->m_Context.Ebp, pEvent->m_Context.Esi, pEvent->m_Context.Edi,
                                pEvent->m_Context.Eip, 
                                pEvent->m_Context.SegCs, pEvent->m_Context.SegDs, pEvent->m_Context.SegEs,
                                pEvent->m_Context.SegSs, pEvent->m_Context.SegFs, dwTIB,
                                eflg.OF, eflg.DF, eflg.IF, eflg.TF, eflg.SF, 
                                eflg.ZF, eflg.AF, eflg.PF, eflg.CF);
    
    pEvent->m_pUI->ShowInfo(g_szBuf);  
    
    ShowTwoASM(pEvent);
}

/************************************************************************/
/* 
Function : get one instruction pointed by specified dwAddr or eip (if dwAddr not set)

Params   : pEvent used to take care of modification of BreakPoint
           dwAddr used to indicate where to start decoding, 
                if null, indicate the eip
           pnCodeSize used to receive the instruction size
                if do not need, can be set to NULL

Process : take care of modification of BreakPoint, like 0x0CC
          remember to restore b4 show to user
          */
/************************************************************************/
const char * 
CExceptEvent::GetOneASM(CBaseEvent *pEvent,
                       DWORD dwAddr/*=NULL*/, 
                       UINT *pnCodeSize/*=NULL*/)
{
    assert(pEvent != NULL);
    UINT nCodeSize;
    BOOL bRet;

    DWORD dwCodeAddr = pEvent->m_Context.Eip;
    if (dwAddr != NULL)
    {
        dwCodeAddr = dwAddr;
    }
    
    bRet = ReadBuf(pEvent,
                   pEvent->m_hProcess, 
                   (LPVOID)dwCodeAddr,
                   gs_szCodeBuf,
                   sizeof(gs_szCodeBuf)
                   );    
    if (!bRet)
    {
        CUI::ShowErrorMessage();
    }

    
    //only care about the first code 
    tagNormalBP *pNormalBP = NULL;
    bRet = HasNormalBP(pEvent, dwCodeAddr, &pNormalBP);
    if (bRet)
    {
        assert(pNormalBP != NULL);
        gs_szCodeBuf[0] = pNormalBP->oldvalue;
    }
    
    Decode2AsmOpcode((PBYTE)gs_szCodeBuf,
                    gs_szASM,
                    gs_szOpcode, 
                    &nCodeSize,
                    dwCodeAddr);

    //receive the code size
    if (pnCodeSize != NULL)
    {
        *pnCodeSize = nCodeSize;
    }
    
    _snprintf(g_szBuf, MAXBUF, "%p:  %-16s   %-16s   [%d]\r\n",
                                dwCodeAddr, 
                                gs_szOpcode, 
                                gs_szASM, 
                                nCodeSize);

    return g_szBuf;
}

/************************************************************************/
/*  
Function : see GetOneAsm                                                  */
/************************************************************************/
const char * 
CExceptEvent::ShowOneASM(CBaseEvent *pEvent,
                        DWORD dwAddr/*=NULL*/, 
                       UINT *pnCodeSize/*=NULL*/)
{
    GetOneASM(pEvent, dwAddr, pnCodeSize);
    CUI::ShowInfo(g_szBuf);
    return g_szBuf;
}

/************************************************************************/
/* 
Function: used to show two instructions pointed by dwAddr or eip (if dwAddr not set)

Params  : pEvent usually is CExceptionEvent object, 
            used to take care of code modified for BreakPoint
                                                                     */
/************************************************************************/
void 
CExceptEvent::ShowTwoASM(CBaseEvent *pEvent, 
                       DWORD dwAddr/*=NULL*/)
{
    assert(pEvent != NULL);

    DWORD dwCodeAddr = pEvent->m_Context.Eip;
    if (dwAddr != NULL)
    {   
        dwCodeAddr = dwAddr;       
    }

    UINT nCodeSize;
    ShowOneASM(pEvent, dwCodeAddr, &nCodeSize);
    ShowOneASM(pEvent, dwCodeAddr + nCodeSize);
}

/************************************************************************/
/* 
Function : show 8 instructions pointed by specified addr or eip(if no addr specified)
*/
/************************************************************************/
BOOL
CExceptEvent::DoShowASM(CBaseEvent *pEvent, int argc, int pargv[], const char *pszBuf)
{ 
    //u or u addr
    assert(pEvent != NULL);
    assert(pszBuf != NULL);

    static DWORD dwLastAddr = pEvent->m_Context.Eip;

    DWORD dwCodeAddr = pEvent->m_Context.Eip;
    if (2 == argc)
    {   
        dwCodeAddr = strtoul(&pszBuf[pargv[1]], NULL, 16);
        assert(dwCodeAddr != ULONG_MAX);
    }
    else
    {
        dwCodeAddr = dwLastAddr;
    }

    UINT nCodeSize;
    for (int i = 0; i < 8; i++)
    {        
        ShowOneASM(pEvent, dwCodeAddr, &nCodeSize);
        dwCodeAddr += nCodeSize;
    }

    dwLastAddr = dwCodeAddr;
    return TRUE;
}

BOOL
CExceptEvent::DoShowData(CBaseEvent *pEvent, int argc, int pargv[], const char *pszBuf)
{ 
    //d or d addr
    assert(pEvent != NULL);
    assert(pszBuf != NULL);

    static DWORD dwLastAddr = pEvent->m_Context.Eip;
    static tagNormalBP *pNormalBP;

    DWORD dwDataAddr = NULL;
    if (2 == argc)
    {   
        dwDataAddr = strtoul(&pszBuf[pargv[1]], NULL, 16);
        assert(dwDataAddr != ULONG_MAX);
    }
    else
    {
        dwDataAddr = dwLastAddr;
    }
    
    //now try to read 128byte
#define MAXREAD  128
#define MAXLINE   16
    static unsigned char pBuf[MAXREAD];
    DWORD nRead = NULL;
    BOOL bRet = ReadProcessMemory(pEvent->m_hProcess,
                                  (LPVOID)dwDataAddr,
                                  pBuf,
                                  MAXREAD,
                                  &nRead);
    if (!bRet)
    {
        CUI::ShowErrorMessage();
    }

    //update record
    dwLastAddr = dwDataAddr + nRead;

    //format and show
    int i = 0;
    int j = 0;
    sprintf(g_szBuf, "%p  ", dwDataAddr);
    for (i = 0; i < MAXREAD; i++, dwDataAddr++)
    {
        //whether modified by BreakPoint
        bRet = HasNormalBP(pEvent, dwDataAddr, &pNormalBP);
        if (bRet)
        {
            pBuf[i] = pNormalBP->oldvalue;
        }

        _snprintf(g_szBuf, MAXBUF, "%s%02X ",
                                    g_szBuf,
                                    pBuf[i]);
        if (0 == (i + 1) % MAXLINE
            && i != 0
            && i != MAXREAD - 1)
        {
            //show ascii
            _snprintf(g_szBuf, MAXBUF, "%s  ", g_szBuf);
            for (j = i - MAXLINE + 1; j <= i; j++)
            {
                if (isprint(pBuf[j]))
                {
                    _snprintf(g_szBuf, MAXBUF, "%s%c", g_szBuf, pBuf[j]);
                }
                else
                {
                    _snprintf(g_szBuf, MAXBUF, "%s.", g_szBuf);
                }
            }       

            //next line
            _snprintf(g_szBuf, MAXBUF, "%s\r\n%p  ",
                                        g_szBuf,
                                        dwDataAddr + 1);
        }
    }

    _snprintf(g_szBuf, MAXBUF, "%s\r\n", g_szBuf);
    pEvent->m_pUI->ShowInfo(g_szBuf);    
    return TRUE;
}

/************************************************************************/
/* 
Function : enable the trace function, trace the instruction within a range */
/************************************************************************/
BOOL 
CExceptEvent::DoTrace(CBaseEvent *pEvent, int argc, int pargv[], const char *pszBuf)
{
    //trace addrstart  addrend [dll]
    assert(argc >= 3);
    assert(pEvent != NULL);
    assert(pszBuf != NULL);

    DWORD dwAddrStart = strtoul(&pszBuf[pargv[1]], NULL, 16);
    DWORD dwAddrEnd   = strtoul(&pszBuf[pargv[2]], NULL, 16);

    //actually no need to check this, we can use VirtualQuery by MemBP
    if (dwAddrStart < 0x10000
        || dwAddrStart >= 0x80000000
        || dwAddrEnd < 0x10000
        || dwAddrEnd >= 0x80000000
        || dwAddrStart > dwAddrEnd)
    {
        pEvent->m_pUI->ShowInfo("Invalid Trace Range\r\n");
        return FALSE;
    }

    //set MemBP,  bm 00400000 a len
    int argv[] = {0, 3, 0x0C, 0x0E};
    sprintf(g_szBuf, "bm %p a %d", dwAddrStart, dwAddrEnd - dwAddrStart);
    DoBM(pEvent, 4, argv, g_szBuf, TRUE);

    return TRUE;
}

/************************************************************************/
/* 
Function : get TIB by FS                                                                     */
/************************************************************************/
DWORD 
CExceptEvent::GetTIB(CBaseEvent *pEvent)
{
    assert(pEvent != NULL);

    LDT_ENTRY ldtSelectorEntry;
    BOOL bRet = GetThreadSelectorEntry(
                        pEvent->m_hThread,
                        pEvent->m_Context.SegFs,
                        &ldtSelectorEntry);
    if (!bRet)
    {
        CUI::ShowErrorMessage();
        return 0;
    }

    //BaseHi(BYTE)  BaseMid(BYTE)  BaseLow(WORD)
    //32  24  16  8  0
    DWORD dwRet = 0;
    dwRet = ldtSelectorEntry.BaseLow;
    dwRet += (ldtSelectorEntry.HighWord.Bytes.BaseMid << 16);
    dwRet += (ldtSelectorEntry.HighWord.Bytes.BaseHi << 24);
    return dwRet;
}

/************************************************************************/
/* 
Function : show the SEH Chain                                                                     */
/************************************************************************/
BOOL 
CExceptEvent::DoShowSEH(CBaseEvent *pEvent, int argc, int pargv[], const char *pszBuf)
{
    assert(pEvent != NULL);

    DWORD dwTIB = GetTIB(pEvent);
    if (0 == dwTIB)
    {
        return FALSE;
    }

    BOOL bRet;
    tagSEH seh;

    bRet = ReadBuf(pEvent,
                    pEvent->m_hProcess,
                  (LPVOID)dwTIB,
                  &seh,
                  sizeof(tagSEH)
                  );
    if (!bRet)
    {
        return FALSE;
    }

    //FS:[0]---> Pointer to Next SEH Record, 
    //           SEH Handler
    BOOL bTopmost = TRUE;
    tagSEH *pSEH = (tagSEH *)(seh.ptrNext);
    do 
    {
        bRet = ReadBuf(pEvent,
                        pEvent->m_hProcess,
                       (LPVOID)pSEH,
                       &seh,
                       sizeof(tagSEH)
                       );
        if (!bRet)
        {
            break;
        }

        //set normal bp and MEMBP at the topmost seh handler
        //but not a good idea to check within a loop, low efficiency
        if (/*bTopmost*/FALSE)
        {
            bTopmost = FALSE;

            //bp addr
            int argv[] = {0, 3};
            sprintf(g_szBuf, "bp %p", seh.dwHandler);
            pEvent->m_bTmpBP = TRUE;
            DoBP(pEvent, 2, argv, g_szBuf);

            //bm addr a len  (how long is okay??)
            int argv1[] = {0, 3, 0x0C, 0x0E};       //this can be a const, used many times
            sprintf(g_szBuf, "bm %p a 4", seh.dwHandler);
            DoBM(pEvent, 4, argv1, g_szBuf, TRUE);

            sprintf(g_szBuf, "SEH Chain Updated*******\r\n");
        }

        _snprintf(g_szBuf, MAXBUF, "%sAddress: %p   SEH Handler: %p\r\n",
                                  g_szBuf,
                                  pSEH,
                                  seh.dwHandler
                                  );
        pSEH = (tagSEH *)seh.ptrNext;
    } while ((DWORD)pSEH != 0xFFFFFFFF);

    _snprintf(g_szBuf, MAXBUF, "%s\r\n", g_szBuf);
    pEvent->m_pUI->ShowInfo(g_szBuf);
  
    return TRUE;
}

/************************************************************************/
/* 
Function : set memory breakpoint on the thread's FS:[0]
           to monitor change of SEH Chain.
           Be helpful for trace   

Remarks  : we use HardWare BreakPoint to monitor,
           so less DRX                                                                  */
/************************************************************************/
BOOL 
CExceptEvent::MonitorSEH(CBaseEvent *pEvent)
{
    assert(pEvent != NULL);

    DWORD dwTIB = GetTIB(pEvent);
    if (0 == dwTIB)
    {
        return FALSE;
    }

    //bh addr w 4
    int argv[] = {0, 3, 0x0C, 0x0E};
    sprintf(g_szBuf, "bh %p w 4", dwTIB);
    DoBH(pEvent, 4, argv, g_szBuf);

    return TRUE;
}

//////////////////////////////////////////////////////////////////////////
/************************************************************************/
/* 
Function : judge whether is call instructions pointed by eip
Params   : pnLen used to receive the instruction size 
Return   : TRUE if is call, FALSE otherwise

004012B6  |.  FF15 A8514200 CALL DWORD PTR DS:[<&KERNEL32.GetVersion>;  kernel32.GetVersion
0040130E  |.  E8 9D2A0000   CALL testDbg.00403DB0                    ; \testDbg.00403DB0
  
*/
/************************************************************************/
BOOL
CExceptEvent::IsCall(CBaseEvent *pEvent, DWORD *pnLen)
{
    assert(pEvent != NULL);
    assert(pnLen != NULL);

    static char szCodeBuf[64];
    static char szOpcode[64];
    static char szASM[128];
    UINT nCodeSize;
     
    //not a good idea to use EIP as default, not universal...., but makes the caller easier
    BOOL bRet = ReadBuf(pEvent,
                        pEvent->m_hProcess, 
                        (LPVOID)pEvent->m_Context.Eip,
                        szCodeBuf, 
                        sizeof(szCodeBuf));    
    if (!bRet)
    {
        return FALSE;
    }
    
    Decode2AsmOpcode((PBYTE)szCodeBuf,
                    szASM,
                    szOpcode, 
                    &nCodeSize,
                    pEvent->m_Context.Eip);

    *pnLen = nCodeSize;

    if (0 == memcmp(szOpcode, "E8", 2)
        || 0 == memcmp(szOpcode, "FF15", 4)
        //others
        )
    {
        return TRUE;
    }

    return FALSE;
}

/************************************************************************/
/*  
Function : read process memory (hProcess)
          need to considering MemBP
          pEvent used to take care of MemBP

Remarks  : Be care that we disable and re-enable the MemBP by ourself.
                                                                    */
/************************************************************************/
BOOL 
CExceptEvent::ReadBuf(CBaseEvent *pEvent, HANDLE hProcess, LPVOID lpAddr, LPVOID lpBuf, SIZE_T nSize)
{
    assert(pEvent != NULL);

    //also need to consider MemBP
    //whether exists memory BP
    tagPageBP *ppageBP = NULL;
    DWORD dwOldProtect;
    BOOL  bHasMemBP = FALSE;
    BOOL bRet = HasMemBP(pEvent, (DWORD)lpAddr, &ppageBP);
    if (bRet)
    {
        bHasMemBP = TRUE;

        //need to restore the protect, (and add PAGE_READWRITE)
        bRet = VirtualProtectEx(pEvent->m_hProcess,
                                (LPVOID)lpAddr,
                                nSize,
                                ppageBP->dwOldProtect,
                                &dwOldProtect
                                );
        if (!bRet)
        {
            CUI::ShowErrorMessage();
            return FALSE;
        }
    }

    //
    bRet = ReadProcessMemory(
                    hProcess, 
                    lpAddr,
                    lpBuf,
                    nSize,
                    NULL);
    
    if (!bRet)
    {
        CUI::ShowErrorMessage();
        return FALSE;
    }

    //now re-enable Membp
    if (bHasMemBP)
    {
        bRet = VirtualProtectEx(pEvent->m_hProcess,
                                (LPVOID)lpAddr,
                                nSize,
                                ppageBP->dwNewProtect,
                                &dwOldProtect
                                );
        if (!bRet)
        {
            CUI::ShowErrorMessage();
            return FALSE;
        }   
    }

    return TRUE;
}

/************************************************************************/
/*
Function : remove the MemBP used for trace
           usually called when the dll unloaded                                                                     */
/************************************************************************/
BOOL 
CExceptEvent::RemoveTrace(CBaseEvent *pEvent, tagModule *pModule)
{
    //bm addr a len
    //bmc id
    assert(pEvent != NULL);
    assert(pModule != NULL);

    //find id first, 
    DWORD dwAddr = pModule->dwBaseOfCode;
    DWORD dwSize = pModule->dwSizeOfCode;
    tagMemBP *pMemBP = NULL;
    int argv[] = {0, 4};    //bmc id
    int i = 0;
    
    list<tagMemBP>::iterator itMemBP;
    for (itMemBP = m_lstMemBP.begin();
         itMemBP != m_lstMemBP.end();
         itMemBP++, i++)
    {
        pMemBP = &(*itMemBP);
        if (pMemBP->dwAddr == dwAddr
            && pMemBP->dwSize == dwSize
            && pMemBP->bTrace)
        {
            sprintf(g_szBuf, "bmc %d", i);
            ((CUseDebugger *)pEvent)->DoBMC(2, argv, g_szBuf);
            break;
        }
    }
    
    return TRUE;
}                     
