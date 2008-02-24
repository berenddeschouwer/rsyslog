/* var.c - a typeless variable class
 *
 * This class is used to represent variable values, which may have any type.
 * Among others, it will be used inside rsyslog's expression system, but
 * also internally at any place where a typeless variable is needed.
 *
 * Module begun 2008-02-20 by Rainer Gerhards, with some code taken
 * from the obj.c/.h files.
 *
 * Copyright 2007, 2008 Rainer Gerhards and Adiscon GmbH.
 *
 * This file is part of rsyslog.
 *
 * Rsyslog is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Rsyslog is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Rsyslog.  If not, see <http://www.gnu.org/licenses/>.
 *
 * A copy of the GPL can be found in the file "COPYING" in this distribution.
 */

#include "config.h"
#include <stdlib.h>
#include <assert.h>

#include "rsyslog.h"
#include "obj.h"
#include "var.h"

/* static data */
DEFobjStaticHelpers


/* Standard-Constructor
 */
BEGINobjConstruct(var) /* be sure to specify the object type also in END macro! */
ENDobjConstruct(var)


/* ConstructionFinalizer
 * rgerhards, 2008-01-09
 */
rsRetVal varConstructFinalize(var_t __attribute__((unused)) *pThis)
{
	DEFiRet;

	ISOBJ_TYPE_assert(pThis, var);

	RETiRet;
}


/* destructor for the var object */
BEGINobjDestruct(var) /* be sure to specify the object type also in END and CODESTART macros! */
CODESTARTobjDestruct(var)
	if(pThis->pcsName != NULL)
		d_free(pThis->pcsName);
	if(pThis->varType == VARTYPE_STR) {
		if(pThis->val.pStr != NULL)
			d_free(pThis->val.pStr);
	}

ENDobjDestruct(var)


/* DebugPrint support for the var object */
BEGINobjDebugPrint(var) /* be sure to specify the object type also in END and CODESTART macros! */
CODESTARTobjDebugPrint(var)
	switch(pThis->varType) {
		case VARTYPE_STR:
			dbgoprint((obj_t*) pThis, "type: cstr, val '%s'\n", rsCStrGetSzStr(pThis->val.pStr));
			break;
		case VARTYPE_NUMBER:
			dbgoprint((obj_t*) pThis, "type: int64, val %lld\n", pThis->val.num);
			break;
		default:
			dbgoprint((obj_t*) pThis, "type %d currently not suppored in debug output\n", pThis->varType);
			break;
	}
ENDobjDebugPrint(var)


/* free the current values (destructs objects if necessary)
 */
static rsRetVal
varUnsetValues(var_t *pThis)
{
	DEFiRet;

	ISOBJ_TYPE_assert(pThis, var);
	if(pThis->varType == VARTYPE_STR)
		rsCStrDestruct(&pThis->val.pStr);

	pThis->varType = VARTYPE_NONE;

	RETiRet;
}


/* set a string value 
 * The caller hands over the string and must n longer use it after this method
 * has been called.
 */
static rsRetVal
varSetString(var_t *pThis, cstr_t *pStr)
{
	DEFiRet;

	ISOBJ_TYPE_assert(pThis, var);

	CHKiRet(varUnsetValues(pThis));
	pThis->varType = VARTYPE_STR;
	pThis->val.pStr = pStr;

finalize_it:
	RETiRet;
}


/* set an int64 value */
static rsRetVal
varSetNumber(var_t *pThis, number_t iVal)
{
	DEFiRet;

	ISOBJ_TYPE_assert(pThis, var);

	CHKiRet(varUnsetValues(pThis));
	pThis->varType = VARTYPE_NUMBER;
	pThis->val.num = iVal;

finalize_it:
	RETiRet;
}


/* check if the provided object can be converted to a number. Uses
 * non-standard calling conventions because it makes an awful lot of sense.
 * Returns 1 if conversion is possibe and 0 if not. If 1 is returned, a
 * conversion request on the unchanged object is guaranteed to succeed.
 * rgerhards, 2008-02-22
 */
rsRetVal
ConvToNumber(var_t *pThis)
{
	DEFiRet;
	number_t n;

	if(pThis->varType == VARTYPE_NUMBER) {
		FINALIZE;
	} else if(pThis->varType == VARTYPE_STR) {
		CHKiRet(rsCStrConvertToNumber(pThis->val.pStr, &n));
		pThis->val.num = n;
		pThis->varType = VARTYPE_NUMBER;
	}

finalize_it:
	RETiRet;
}


/* convert the provided var to type string. This is always possible
 * (except, of course, for things like out of memory...)
 * TODO: finish implementation!!!!!!!!!
 * rgerhards, 2008-02-24
 */
rsRetVal
ConvToString(var_t *pThis)
{
	DEFiRet;

	if(pThis->varType == VARTYPE_STR) {
		FINALIZE;
	} else if(pThis->varType == VARTYPE_NUMBER) {
		//CHKiRet(rsCStrConvertToNumber(pThis->val.pStr, &n));
		//pThis->val.num = n;
		// TODO: ADD CODE!!!!
		pThis->varType = VARTYPE_STR;
	}

finalize_it:
	RETiRet;
}



/* This function is used to prepare two var_t objects for a common operation,
 * e.g before they are added, multiplied or compared. The function looks at
 * the data types of both operands and finds the best data type suitable for
 * the operation (in respect to current types). Then, it converts those
 * operands that need conversion. Please note that the passed-in var objects
 * *are* modified and returned as new type. So do call this function only if
 * you actually need the conversion.
 *
 * This is how the common data type is selected. Note that op1 and op2 are
 * just the two operands, their order is irrelevant (this would just take up
 * more table space - so string/number is the same thing as number/string).
 *
 * Common Types:
 * op1		op2	operation data type
 * string	string	string
 * string	number	number if op1 can be converted to number, string else
 * date		string  date if op1 can be converted to date, string else
 * number	number  number
 * date		number	string (maybe we can do better?)
 * date		date	date
 * none         n/a     error
 *
 * If a boolean value is required, we need to have a number inside the
 * operand. If it is not, conversion rules to number apply. Once we
 * have a number, things get easy: 0 is false, anything else is true.
 * Please note that due to this conversion rules, "0" becomes false
 * while "-4712" becomes true. Using a date as boolen is not a good
 * idea. Depending on the ultimate conversion rules, it may always
 * become true or false. As such, using dates as booleans is
 * prohibited and the result defined to be undefined.
 *
 * rgerhards, 2008-02-22
 */
static rsRetVal
ConvForOperation(var_t *pThis, var_t *pOther)
{
	DEFiRet;
	varType_t commonType;

	if(pThis->varType == VARTYPE_NONE || pOther->varType == VARTYPE_NONE)
		ABORT_FINALIZE(RS_RET_INVALID_VAR);

	switch(pThis->varType) {
		case VARTYPE_NONE:
			ABORT_FINALIZE(RS_RET_INVALID_VAR);
			break;
		case VARTYPE_STR:
			switch(pOther->varType) {
				case VARTYPE_NONE:
					ABORT_FINALIZE(RS_RET_INVALID_VAR);
					break;
				case VARTYPE_STR:
					/* two strings, we are all set */
					break;
				case VARTYPE_NUMBER:
					/* check if we can convert pThis to a number, if so use number format. */
					iRet = ConvToNumber(pThis);
					if(iRet != RS_RET_NOT_A_NUMBER) {
						CHKiRet(ConvToString(pOther));
					} else {
						FINALIZE; /* OK or error */
					}
					break;
				case VARTYPE_SYSLOGTIME:
					ABORT_FINALIZE(RS_RET_NOT_IMPLEMENTED);
					break;
			}
			break;
		case VARTYPE_NUMBER:
			switch(pOther->varType) {
				case VARTYPE_NONE:
					ABORT_FINALIZE(RS_RET_INVALID_VAR);
					break;
				case VARTYPE_STR:
					iRet = ConvToNumber(pOther);
					if(iRet != RS_RET_NOT_A_NUMBER) {
						CHKiRet(ConvToString(pThis));
					} else {
						FINALIZE; /* OK or error */
					}
					break;
				case VARTYPE_NUMBER:
					commonType = VARTYPE_NUMBER;
					break;
				case VARTYPE_SYSLOGTIME:
					ABORT_FINALIZE(RS_RET_NOT_IMPLEMENTED);
					break;
			}
			break;
		case VARTYPE_SYSLOGTIME:
			ABORT_FINALIZE(RS_RET_NOT_IMPLEMENTED);
			break;
	}

finalize_it:
	RETiRet;
}


/* queryInterface function
 * rgerhards, 2008-02-21
 */
BEGINobjQueryInterface(var)
CODESTARTobjQueryInterface(var)
	if(pIf->ifVersion != varCURR_IF_VERSION) { /* check for current version, increment on each change */
		ABORT_FINALIZE(RS_RET_INTERFACE_NOT_SUPPORTED);
	}

	/* ok, we have the right interface, so let's fill it
	 * Please note that we may also do some backwards-compatibility
	 * work here (if we can support an older interface version - that,
	 * of course, also affects the "if" above).
	 */
	pIf->oID = OBJvar;

	pIf->Construct = varConstruct;
	pIf->ConstructFinalize = varConstructFinalize;
	pIf->Destruct = varDestruct;
	pIf->DebugPrint = varDebugPrint;
	pIf->SetNumber = varSetNumber;
	pIf->SetString = varSetString;
	pIf->ConvForOperation = ConvForOperation;
finalize_it:
ENDobjQueryInterface(var)


/* Initialize the var class. Must be called as the very first method
 * before anything else is called inside this class.
 * rgerhards, 2008-02-19
 */
BEGINObjClassInit(var, 1) /* class, version */
	OBJSetMethodHandler(objMethod_DEBUGPRINT, varDebugPrint);
	OBJSetMethodHandler(objMethod_CONSTRUCTION_FINALIZER, varConstructFinalize);
ENDObjClassInit(var)

/* vi:set ai:
 */
