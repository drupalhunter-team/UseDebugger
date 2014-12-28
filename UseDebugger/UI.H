#ifndef UI_H_5B3FA45C_79E7_485d_BEAB_07CFCA68BE92
#define UI_H_5B3FA45C_79E7_485d_BEAB_07CFCA68BE92

#include "Common.h"

/************************************************************************/
/* Encapsulation for UI Related                                         */
/************************************************************************/
class CUI
{
public:
    //display menus
    virtual void ShowMainMenu(void);
    virtual void ShowInfo(const char *pszInfo);

    //get input from user
    virtual void GetCH(char *pch);

    /************************************************************************/
    /* 
    Function : get input from user, and decode 
    Params   : pargc used to know how many words
               pargv used to receive every start poisition in pszBuf
               pszBuf used to receive the input
                  word1 NULL word2 NULL word3 NULL....*/
    /************************************************************************/
    virtual void GetInput(OUT int *pargc, 
                          OUT int pargv[],
                          IN char *pszBuf, 
                          IN SIZE_T nMaxBuf);

    //
    virtual BOOL SelectFile(char *pszPath, SIZE_T nMaxPath = MAX_PATH);
protected:
private:
};

#endif