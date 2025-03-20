
# Simple custom test suite

import inspect

class Test:
    def __init__(self):
        " Simple test suite "
        self.fails = []

    def __new_fail(self, message):
        stack = inspect.stack()
        self.fails.append((stack[2].lineno, stack[1].function, message))
        return False
    
    def __cutoff_value(self, value):
        s = repr(value)

        if len(s) > 125:
            s = s[:125] + ' ... (cut off)'
        
        return s
    

    def equal(self, expected, value):
        " Check if EXPECTED is equal to VALUE "

        if expected != value:
            return self.__new_fail(f"Expected value to be equal to:\n {self.__cutoff_value(expected)}\nBut got value:\n {self.__cutoff_value(value)}")
        
        return True
    
    def unequal(self, not_expected, value):
        " Check if NOT_EXPECTED is unequal to VALUE "

        if not_expected == value:
            return self.__new_fail(f"Expected value to be unequal to:\n {self.__cutoff_value(not_expected)}\nBut got value:\n {self.__cutoff_value(value)}")
        
        return True
    
    def success(self, func):
        " Check if FUNC executes without error "

        try:
            func()
            return True

        except Exception as e:
            return self.__new_fail(f"Failed a function with error: {e}")
    
    def exception(self, func, exception=None):
        " Check if FUNC executes with EXCEPTION "

        try:
            func()

            return self.__new_fail("Successfully called a function while expecting an exception")

        except Exception as e:
            if exception and not isinstance(e, exception):
                return self.__new_fail(f"Successfully failed a function, but with unexpected exception: {type(e)}: {e}")
            
            return True
    
    def print(self):
        nfails = len(self.fails)

        fname = inspect.stack()[1].filename

        if nfails == 0:
            print(f"No fails in '{fname}'.")
            return

        print(f"Found {nfails} fails in {fname}:")

        for line_no, check_type, fail in self.fails:
            print(f"\n## Line {line_no} ## (func: '{check_type}')\n{fail}")

